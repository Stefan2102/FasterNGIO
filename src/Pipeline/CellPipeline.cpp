#include "Pipeline/CellPipeline.h"

#include "Concurrency/AtomicWait.h"
#include "Grass/CellCache.h"
#include "Pipeline/FileWriterPool.h"
#include "Pipeline/SuspensionWaiters.h"
#include "Pipeline/TbbGraphScheduler.h"
#include "Platform/WholeFile.h"
#include "Rejection/CpuBvh.h"
#include "Rejection/CpuReference.h"
#if FASTERNGIO_HAS_GPU
#include "Gpu/GpuRejector.h"
#endif

#include <ORGModuleServices/Async/StateGraph.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <exception>
#include <functional>
#include <stdexcept>

namespace FasterNGIO::Pipeline
{
	namespace
	{
		using Collision::Float3;
		using Graph = org::async::AsyncStateGraph;
		using Key = Graph::ArtifactKey;
		using BuildContext = Graph::ArtifactBuildContext;
		using BuildResult = Graph::ArtifactBuildResult;

		constexpr std::uint16_t kCellTrace = 0;
		constexpr std::uint16_t kCellOutput = 1;

		struct CellInput
		{
			std::uint32_t cell{ 0 };
		};

		// Everything one cell carries from placement to output. Owned by the CellTrace payload;
		// CellOutput empties it once the file is written so finished cells hold no memory.
		struct CellWork
		{
			bool skip{ false };
			bool cancelled{ false };
			bool holdsCapacity{ false };
			std::string error;
			Grass::CellCandidates candidates;
			std::vector<Rejection::QueryShape> shapes;
			std::vector<std::uint32_t> rejected;
#if FASTERNGIO_HAS_GPU
			std::shared_ptr<Gpu::TraceJob> job;
#endif
		};

		struct CellTraceArtifact
		{
			std::shared_ptr<CellWork> work;
		};

		struct CellDone
		{
		};

		// Cells whose candidates may wait for the GPU at once. Producers past the limit suspend in the
		// graph until a cell is written; nothing blocks.
		constexpr std::uint32_t kMaxCellsAwaitingGpu = 4096;

		// Admission for cells waiting on the GPU. TryAcquire never blocks; a producer that misses
		// registers a waiter and suspends in the graph, and Release wakes exactly one live waiter.
		class CapacityBroker
		{
		public:
			CapacityBroker(std::uint32_t a_capacity, std::function<void(std::uint64_t)> a_notify) :
				_available(a_capacity), _notify(std::move(a_notify)) {}

			[[nodiscard]] bool TryAcquire() noexcept
			{
				auto available = _available.load(std::memory_order_acquire);
				while (available > 0) {
					if (_available.compare_exchange_weak(available, available - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
						return true;
					}
				}
				return false;
			}

			// Returns 0 when capacity was obtained after all, else the identity to suspend on.
			[[nodiscard]] std::uint64_t AcquireOrWait()
			{
				if (TryAcquire()) {
					return 0;
				}
				return _waiters.Wait(_notify, [this] { return TryAcquire(); });
			}

			void Release()
			{
				_available.fetch_add(1, std::memory_order_acq_rel);
				_waiters.WakeOne();
			}

		private:
			std::atomic<std::int64_t> _available;
			SuspensionWaiters::Notify _notify;
			SuspensionWaiters _waiters;
		};

		[[nodiscard]] std::uint64_t CountBits(const std::vector<std::uint32_t>& a_bits)
		{
			std::uint64_t count = 0;
			for (const auto word : a_bits) {
				count += static_cast<std::uint64_t>(std::popcount(word));
			}
			return count;
		}

		class CellPipelineRun
		{
		public:
			explicit CellPipelineRun(const CellPipelineDesc& a_desc) :
				_desc(a_desc),
				_graph(MakeTbbGraphScheduler(), "FasterNGIO", MakeHooks()),
				_capacity(kMaxCellsAwaitingGpu, _graph.MakeSuspensionNotifier()),
				_writeNotify(_graph.MakeSuspensionNotifier())
			{
				_graph.RegisterTypedProducer<CellInput, CellTraceArtifact>(kCellTrace, 0, 0, "FasterNGIO::CellTrace",
					[this](const BuildContext& a_context, std::shared_ptr<const CellInput> a_input) { return BuildTrace(a_context, *a_input); });
				_graph.RegisterTypedProducer<CellInput, CellDone>(kCellOutput, 0, 0, "FasterNGIO::CellOutput",
					[this](const BuildContext& a_context, std::shared_ptr<const CellInput> a_input) { return BuildOutput(a_context, *a_input); });
			}

			~CellPipelineRun() { _graph.Shutdown(); }

			CellPipelineStats Run()
			{
				const auto cellCount = static_cast<std::uint32_t>(_desc.lands.size());
				// Exact (non-coalescible) lock-free posts: each returns its version handle immediately,
				// so the output can name its trace before the graph has applied either request.
				std::vector<Graph::ArtifactRequestResult> requests;
				requests.reserve(static_cast<std::size_t>(cellCount) * 2);
				for (std::uint32_t cell = 0; cell < cellCount; ++cell) {
					auto trace = _graph.PostRequest(Intent(kCellTrace, cell, {}), false);
					if (!trace) {
						throw std::runtime_error("state graph refused a cell trace request");
					}
					auto output = _graph.PostRequest(
						Intent(kCellOutput, cell, { org::async::Exact(trace.version, org::async::ArtifactReadiness::GpuReady) }), false);
					if (!output) {
						throw std::runtime_error("state graph refused a cell output request");
					}
					requests.push_back(std::move(trace));
					requests.push_back(std::move(output));
				}

				// Sleep (futex) until every CellOutput has run; producers count themselves done.
				Concurrency::WaitUntil(_finished, [cellCount](auto a_finished) { return a_finished >= cellCount; });

				CellPipelineStats stats;
				stats.cellsWritten = _written.load();
				stats.cellsSkipped = _skipped.load();
				stats.cellsFailed = _failed.load();
				stats.cellsCancelled = _cancelled.load();
				stats.blades = _blades.load();
				stats.bladesRejected = _rejected.load();
				stats.validationMismatches = _mismatches.load();
				return stats;
			}

		private:
			[[nodiscard]] static org::async::StateGraphHooks<org::async::DefaultStateGraphTypes> MakeHooks()
			{
				org::async::StateGraphHooks<org::async::DefaultStateGraphTypes> hooks;
				// A cell's trace and output are lifecycle steps of one exact request each.
				hooks.artifactPolicies[kCellTrace].allowCoalescing = false;
				hooks.artifactPolicies[kCellOutput].allowCoalescing = false;
				return hooks;
			}

			[[nodiscard]] static Graph::ArtifactIntent Intent(std::uint16_t a_kind, std::uint32_t a_cell, std::vector<Graph::ArtifactRequirement> a_requirements)
			{
				Graph::ArtifactIntent intent;
				intent.key = Key{ a_kind, a_cell, 0 };
				intent.desiredRevision = 1;
				intent.requirements = std::move(a_requirements);
				intent.input = org::async::ArtifactPayload::Make<CellInput>(std::make_shared<const CellInput>(CellInput{ a_cell }));
				intent.requestFingerprint = (static_cast<std::uint64_t>(a_kind) << 32) | (static_cast<std::uint64_t>(a_cell) + 1);
				return intent;
			}

			[[nodiscard]] std::filesystem::path CellPath(std::uint32_t a_cell) const
			{
				const auto& land = *_desc.lands[a_cell];
				return _desc.outputDirectory / Grass::MakeNgioCacheFileName(_desc.worldEditorID, *land.cellX, *land.cellY);
			}

			[[nodiscard]] static BuildResult Ready(std::shared_ptr<CellWork> a_work, std::shared_ptr<const org::async::GpuSubmissionSet> a_gpu = {})
			{
				return BuildResult::Ready(
					org::async::ArtifactPayload::Make<CellTraceArtifact>(std::make_shared<const CellTraceArtifact>(CellTraceArtifact{ std::move(a_work) })),
					std::move(a_gpu));
			}

			// Placement, then rejection: inline on the CPU, or a trace job posted to the render
			// thread whose completion the graph tracks as this artifact's GPU submission.
			BuildResult BuildTrace(const BuildContext&, const CellInput& a_input)
			{
				auto work = std::make_shared<CellWork>();
				try {
					if (_desc.stop.stop_requested()) {
						work->cancelled = true;
						return Ready(std::move(work));
					}
					if (!_desc.overwrite && Grass::ExistingNgioCacheLooksValid(CellPath(a_input.cell))) {
						work->skip = true;
						return Ready(std::move(work));
					}
					// Before anything is held: a cell is not placed while its file would only queue behind a
					// full backlog.
					if (_desc.writer) {
						if (const auto identity = _desc.writer->AdmitOrWait(_writeNotify); identity != 0) {
							return BuildResult::Suspend(Graph::ArtifactSuspension::Capacity(identity, "the cache writers are behind"));
						}
					}
					if (_desc.backend == RejectionBackend::Gpu) {
						if (const auto identity = _capacity.AcquireOrWait(); identity != 0) {
							return BuildResult::Suspend(Graph::ArtifactSuspension::Capacity(identity, "cells awaiting the GPU are at their limit"));
						}
						work->holdsCapacity = true;
					}
					work->candidates = Grass::GenerateCellCandidates(*_desc.snapshot, *_desc.lands[a_input.cell], _desc.placement);
					work->shapes.reserve(work->candidates.groups.size());
					for (const auto& group : work->candidates.groups) {
						work->shapes.push_back(_desc.shapesByGrass->at(group.grass->formID));
					}
					if (_desc.backend == RejectionBackend::Cpu) {
						work->rejected = _desc.cpuBvh->RejectCell(work->candidates, work->shapes);
						if (_desc.validateCpu) {
							Validate(*work);
						}
					}
#if FASTERNGIO_HAS_GPU
					if (_desc.backend == RejectionBackend::Gpu) {
						return PostToGpu(std::move(work));
					}
#endif
				} catch (const std::exception& e) {
					work->error = e.what();
				}
				return Ready(std::move(work));
			}

#if FASTERNGIO_HAS_GPU
			BuildResult PostToGpu(std::shared_ptr<CellWork> a_work)
			{
				auto queries = Gpu::MakeQueries(a_work->candidates.blades, a_work->shapes);
				if (queries.empty()) {
					return Ready(std::move(a_work));
				}
				auto job = std::make_shared<Gpu::TraceJob>(std::move(queries));
				auto token = std::make_shared<org::async::GpuSubmissionSet>();
				token->isSubmitted = [] { return true; };
				token->isComplete = [job] { return job->Complete(); };
				token->subscribe = [job](std::function<void()> a_callback) { job->Subscribe(std::move(a_callback)); };
				token->completionNotificationsAreAuthoritative = true;
				a_work->job = job;
				_desc.gpu->Post(std::move(job));
				return Ready(std::move(a_work), std::move(token));
			}
#endif

			void CountFinished()
			{
				if (_desc.progress) {
					_desc.progress->fetch_add(1, std::memory_order_relaxed);
				}
				_finished.fetch_add(1, std::memory_order_acq_rel);
				_finished.notify_all();
			}

			void Finish(CellWork& a_work)
			{
				if (a_work.holdsCapacity) {
					a_work.holdsCapacity = false;
					_capacity.Release();
				}
				a_work = CellWork{};
				CountFinished();
			}

			BuildResult BuildOutput(const BuildContext& a_context, const CellInput& a_input)
			{
				std::shared_ptr<const CellTraceArtifact> trace;
				for (const auto& dependency : a_context.dependencies) {
					if (dependency.key.kind == kCellTrace) {
						trace = dependency.payload.Get<CellTraceArtifact>();
					}
				}
				const auto done = BuildResult::Ready(org::async::ArtifactPayload::Make<CellDone>(std::make_shared<const CellDone>()));
				if (!trace || !trace->work) {
					_failed.fetch_add(1);
					CountFinished();
					return done;
				}
				auto& work = *trace->work;
				try {
					if (work.skip) {
						_skipped.fetch_add(1);
						Finish(work);
						return done;
					}
					if (work.cancelled) {
						_cancelled.fetch_add(1);
						Finish(work);
						return done;
					}
#if FASTERNGIO_HAS_GPU
					if (work.job) {
						if (work.job->Failed()) {
							throw std::runtime_error("GPU trace failed");
						}
						const auto hits = work.job->Hits();
						work.rejected.assign((work.candidates.blades.size() + 31) / 32, 0u);
						for (std::size_t b = 0; b < hits.size(); ++b) {
							if (hits[b] != 0) {
								work.rejected[b / 32] |= 1u << (b % 32);
							}
						}
						if (_desc.validateCpu) {
							Validate(work);
						}
					}
#endif
					if (!work.error.empty()) {
						throw std::runtime_error(work.error);
					}
					_rejected.fetch_add(CountBits(work.rejected));
					_blades.fetch_add(work.candidates.blades.size());
					auto bytes = Grass::SerializeNgioCellCache(Grass::FinalizeCell(work.candidates, work.rejected));
					if (_desc.writer) {
						_desc.writer->Submit(CellPath(a_input.cell), std::move(bytes), _desc.writeTally);
					} else {
						Platform::WriteWholeFile(CellPath(a_input.cell), bytes);
					}
					_written.fetch_add(1);
				} catch (const std::exception& e) {
					spdlog::error("cell {}: {}", CellPath(a_input.cell).filename().string(), e.what());
					_failed.fetch_add(1);
				}
				Finish(work);
				return done;
			}

			void Validate(const CellWork& a_work)
			{
				const auto reference = Rejection::RejectCellOnCpu(*_desc.world, a_work.candidates, a_work.shapes);
				for (std::size_t w = 0; w < reference.size(); ++w) {
					const auto diff = reference[w] ^ a_work.rejected[w];
					if (diff == 0) {
						continue;
					}
					const auto previous = _mismatches.fetch_add(static_cast<std::uint64_t>(std::popcount(diff)));
					if (previous < 20) {
						const auto bit = static_cast<std::uint32_t>(std::countr_zero(diff));
						const auto& blade = a_work.candidates.blades[w * 32 + bit];
						const auto& shape = a_work.shapes[blade.groupIndex];
						const auto [p, q] = Rejection::BladeSegment(shape, blade.position);
						// Queries are in blade order.
						const auto query = w * 32 + bit;
						spdlog::warn("mismatch cell ({}, {}) query {} blade at ({:.1f}, {:.1f}, {:.1f}) r={:.2f}: reference={} {}={} {}",
							a_work.candidates.cellX,
							a_work.candidates.cellY,
							query,
							blade.position[0],
							blade.position[1],
							blade.position[2],
							shape.radius,
							(reference[w] >> bit) & 1u,
							_desc.backend == RejectionBackend::Gpu ? "gpu" : "bvh",
							(a_work.rejected[w] >> bit) & 1u,
							Rejection::ExplainCapsule(*_desc.world, a_work.candidates.cellX, a_work.candidates.cellY, p, q, shape.radius));
					}
				}
			}

			const CellPipelineDesc& _desc;
			Graph _graph;
			CapacityBroker _capacity;
			std::function<void(std::uint64_t)> _writeNotify;
			std::atomic<std::uint32_t> _finished{ 0 };
			std::atomic<std::uint64_t> _written{ 0 };
			std::atomic<std::uint64_t> _skipped{ 0 };
			std::atomic<std::uint64_t> _failed{ 0 };
			std::atomic<std::uint64_t> _cancelled{ 0 };
			std::atomic<std::uint64_t> _blades{ 0 };
			std::atomic<std::uint64_t> _rejected{ 0 };
			std::atomic<std::uint64_t> _mismatches{ 0 };
		};
	}

	CellPipelineStats RunCellPipeline(const CellPipelineDesc& a_desc)
	{
		CellPipelineRun pipeline(a_desc);
		return pipeline.Run();
	}
}
