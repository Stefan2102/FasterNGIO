#include "Gpu/GpuRejector.h"

#include "Pipeline/MpscQueue.h"

#include <OpenRenderGraph/PersistentGraphHost.h>
#include <ORGModuleServices/ShaderCompiler.h>
#include <Render/Runtime/RuntimeDevice.h>
#include <Render/Runtime/ThreadPoolTaskService.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/Buffers/Buffer.h>
#include <rhi.h>
#include <rhi_feature_info.h>
#include <rhi_helpers.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <future>
#include <variant>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <thread>

namespace FasterNGIO::Gpu
{
	namespace
	{
		using Collision::CollisionModel;
		using Collision::Float3;

		// Primitive kinds, in BLAS geometry order. Each has its own procedural hit group.
		enum Kind : std::uint32_t
		{
			KindTriangles = 0,
			KindHulls = 1,
			KindCapsules = 2,
			KindCount = 3
		};

		constexpr std::uint64_t kAccelerationStructureAlignment = 256;  // D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT
		constexpr std::uint64_t kBlasScratchBudget = 512ull << 20;
		// One raw SRV per collision model plus a few per frame set. D3D12 caps a shader-visible heap at
		// 1,000,000 descriptors; a Vulkan descriptor heap is a buffer, so stay well inside both.
		constexpr std::uint32_t kDescriptorCapacity = 1u << 17;
		constexpr std::uint32_t kRootConstantCount = 7;
		constexpr std::uint64_t kDebugBytes = 16 + 255 * 96;

		[[nodiscard]] std::uint64_t AlignUp(std::uint64_t a_value, std::uint64_t a_alignment)
		{
			return (a_value + a_alignment - 1) & ~(a_alignment - 1);
		}

		void Check(rhi::Result a_result, const char* a_what)
		{
			if (!rhi::IsOk(a_result)) {
				throw std::runtime_error(std::string("GPU: ") + a_what + " failed (rhi::Result " + std::to_string(static_cast<std::uint32_t>(a_result)) + ")");
			}
		}

		// ---- Model packing ------------------------------------------------------------------

		struct Aabb
		{
			float min[3];
			float max[3];
		};
		static_assert(sizeof(Aabb) == 24);

		struct PackedModel
		{
			std::vector<std::byte> bytes;
			struct Geometry
			{
				Kind kind;
				std::uint64_t aabbOffset;
				std::uint32_t count;
			};
			std::vector<Geometry> geometries;
			std::uint32_t kindMask{ 0 };
		};

		class Writer
		{
		public:
			explicit Writer(std::vector<std::byte>& a_bytes) :
				_bytes(a_bytes) {}

			[[nodiscard]] std::uint64_t Offset() const { return _bytes.size(); }
			void Align(std::uint64_t a_alignment) { _bytes.resize(AlignUp(_bytes.size(), a_alignment)); }

			template <class T>
			void Put(const T& a_value)
			{
				const auto offset = _bytes.size();
				_bytes.resize(offset + sizeof(T));
				std::memcpy(_bytes.data() + offset, &a_value, sizeof(T));
			}

			void PutPoint(const Float3& a_point)
			{
				Put(a_point.x);
				Put(a_point.y);
				Put(a_point.z);
			}

			template <class T>
			void PutAt(std::uint64_t a_offset, const T& a_value)
			{
				std::memcpy(_bytes.data() + a_offset, &a_value, sizeof(T));
			}

		private:
			std::vector<std::byte>& _bytes;
		};

		void Grow(Aabb& a_box, const Float3& a_point, float a_reach)
		{
			a_box.min[0] = (std::min)(a_box.min[0], a_point.x - a_reach);
			a_box.min[1] = (std::min)(a_box.min[1], a_point.y - a_reach);
			a_box.min[2] = (std::min)(a_box.min[2], a_point.z - a_reach);
			a_box.max[0] = (std::max)(a_box.max[0], a_point.x + a_reach);
			a_box.max[1] = (std::max)(a_box.max[1], a_point.y + a_reach);
			a_box.max[2] = (std::max)(a_box.max[2], a_point.z + a_reach);
		}

		[[nodiscard]] Aabb EmptyBox()
		{
			constexpr auto inf = (std::numeric_limits<float>::max)();
			return { { inf, inf, inf }, { -inf, -inf, -inf } };
		}

		// Lays one model out as GrassRejection.hlsl expects, followed by the BLAS AABBs (each
		// primitive's bounds grown by its own radius plus the query inflation).
		[[nodiscard]] PackedModel PackModel(const CollisionModel& a_model, float a_inflation)
		{
			PackedModel packed;
			Writer out(packed.bytes);
			for (int i = 0; i < 8; ++i) {
				out.Put(std::uint32_t{ 0 });
			}

			const auto trianglesOffset = out.Offset();
			for (const auto& tri : a_model.triangles) {
				for (const auto& v : tri.vertices) {
					out.PutPoint(v);
				}
				out.Put(tri.radius);
			}

			const auto hullsOffset = out.Offset();
			const auto hullRecords = out.Offset();
			for (std::size_t i = 0; i < a_model.hulls.size(); ++i) {
				for (int w = 0; w < 8; ++w) {
					out.Put(std::uint32_t{ 0 });
				}
			}
			for (std::size_t i = 0; i < a_model.hulls.size(); ++i) {
				const auto& hull = a_model.hulls[i];
				out.Align(16);
				const auto planeOffset = out.Offset();
				for (std::uint32_t p = 0; p < hull.planeCount; ++p) {
					const auto& plane = a_model.hullPlanes[hull.firstPlane + p];
					out.Put(plane.x);
					out.Put(plane.y);
					out.Put(plane.z);
					out.Put(plane.w);
				}
				const auto triangleOffset = out.Offset();
				for (std::uint32_t t = 0; t < hull.triangleCount; ++t) {
					for (const auto& v : a_model.hullTriangles[hull.firstTriangle + t].vertices) {
						out.PutPoint(v);
					}
				}
				const auto record = hullRecords + i * 32;
				out.PutAt(record + 0, static_cast<std::uint32_t>(planeOffset));
				out.PutAt(record + 4, hull.planeCount);
				out.PutAt(record + 8, static_cast<std::uint32_t>(triangleOffset));
				out.PutAt(record + 12, hull.triangleCount);
				out.PutAt(record + 16, hull.radius);
			}

			out.Align(16);
			const auto capsulesOffset = out.Offset();
			for (const auto& capsule : a_model.capsules) {
				out.PutPoint(capsule.p0);
				out.PutPoint(capsule.p1);
				out.Put(capsule.radius);
				out.Put(0.0f);
			}

			out.PutAt(0, static_cast<std::uint32_t>(trianglesOffset));
			out.PutAt(4, static_cast<std::uint32_t>(a_model.triangles.size()));
			out.PutAt(8, static_cast<std::uint32_t>(hullsOffset));
			out.PutAt(12, static_cast<std::uint32_t>(a_model.hulls.size()));
			out.PutAt(16, static_cast<std::uint32_t>(capsulesOffset));
			out.PutAt(20, static_cast<std::uint32_t>(a_model.capsules.size()));

			const auto beginGeometry = [&](Kind a_kind, std::size_t a_count) {
				out.Align(16);
				packed.geometries.push_back({ a_kind, out.Offset(), static_cast<std::uint32_t>(a_count) });
				packed.kindMask |= 1u << a_kind;
			};
			if (!a_model.triangles.empty()) {
				beginGeometry(KindTriangles, a_model.triangles.size());
				for (const auto& tri : a_model.triangles) {
					auto box = EmptyBox();
					for (const auto& v : tri.vertices) {
						Grow(box, v, tri.radius + a_inflation);
					}
					out.Put(box);
				}
			}
			if (!a_model.hulls.empty()) {
				beginGeometry(KindHulls, a_model.hulls.size());
				for (const auto& hull : a_model.hulls) {
					auto box = EmptyBox();
					Grow(box, hull.aabbMin, hull.radius + a_inflation);
					Grow(box, hull.aabbMax, hull.radius + a_inflation);
					out.Put(box);
				}
			}
			if (!a_model.capsules.empty()) {
				beginGeometry(KindCapsules, a_model.capsules.size());
				for (const auto& capsule : a_model.capsules) {
					auto box = EmptyBox();
					Grow(box, capsule.p0, capsule.radius + a_inflation);
					Grow(box, capsule.p1, capsule.radius + a_inflation);
					out.Put(box);
				}
			}
			return packed;
		}

		// ---- GPU resources ------------------------------------------------------------------

		struct MappedBuffer
		{
			rhi::ResourcePtr resource;
			std::byte* mapped{ nullptr };
			std::uint64_t bytes{ 0 };
		};

		struct GpuModel
		{
			rhi::ResourcePtr data;
			rhi::ResourcePtr staging;
			std::uint64_t dataBytes{ 0 };
			rhi::ResourcePtr blasStorage;
			rhi::AccelerationStructurePtr blas;
			std::uint64_t blasAddress{ 0 };
			std::uint32_t srv{ 0 };
			std::uint32_t kindMask{ 0 };
			std::vector<rhi::RayTracingGeometryDesc> geometries;
			rhi::AccelerationStructurePrebuildInfo prebuild{};
		};

		// Everything one frame's queries need; reused once the frame has completed on the GPU.
		struct FrameSet
		{
			MappedBuffer candidates;
			rhi::ResourcePtr output;
			std::uint64_t outputCapacity{ 0 };
			MappedBuffer readback;
			std::uint32_t candidatesSrv{ 0 };
			std::uint32_t outputUav{ 0 };
			// Diagnostics (FASTERNGIO_DEBUG_CANDIDATE): per-invocation records for one candidate.
			rhi::ResourcePtr debug;
			MappedBuffer debugZero;
			MappedBuffer debugReadback;
			std::uint32_t debugUav{ 0 };
			struct Pending
			{
				std::shared_ptr<TraceJob> job;
				std::uint64_t offset;
				std::uint64_t count;
			};
			std::vector<Pending> pending;
			std::uint64_t queryCount{ 0 };
			bool busy{ false };
		};

		// What the rejection pass records this frame.
		struct FrameWork
		{
			std::vector<rhi::AccelerationStructureBuildDesc> blasBuilds;
			std::vector<std::pair<rhi::ResourceHandle, std::pair<rhi::ResourceHandle, std::uint64_t>>> stagingCopies;
			std::optional<rhi::AccelerationStructureBuildDesc> tlasBuild;
			FrameSet* frame{ nullptr };
		};

		struct PassContext
		{
			rhi::DescriptorHeapHandle heap{};
			rhi::DescriptorHeapHandle samplerHeap{};
			rhi::PipelineLayoutHandle layout{};
			rhi::PipelineHandle pipeline{};
			rhi::RayTracingShaderTableRegion rayGen{};
			rhi::RayTracingShaderTableRegion miss{};
			rhi::RayTracingShaderTableRegion hit{};
			std::uint32_t tlasSrv{ 0 };
			float segmentLength{ 0.0f };
			std::uint32_t debugCandidate{ 0xFFFFFFFFu };
			std::shared_ptr<FrameWork> current;
		};

		void GlobalBarrier(rhi::CommandList& a_commands, rhi::ResourceSyncState a_beforeSync, rhi::ResourceAccessType a_beforeAccess,
			rhi::ResourceSyncState a_afterSync, rhi::ResourceAccessType a_afterAccess)
		{
			rhi::GlobalBarrier barrier{};
			barrier.beforeSync = a_beforeSync;
			barrier.beforeAccess = a_beforeAccess;
			barrier.afterSync = a_afterSync;
			barrier.afterAccess = a_afterAccess;
			rhi::BarrierBatch batch{};
			batch.globals = { &barrier, 1 };
			a_commands.Barriers(batch);
		}

		struct RejectionFrame
		{
			std::shared_ptr<FrameWork> work;
			PassContext context;
		};

		struct RejectionBindings
		{
		};

		// Records BLAS builds, the TLAS build and the frame's one DispatchRays. All
		// synchronisation inside the frame is explicit; the graph only orders whole frames.
		class RejectionPass final : public org::TypedRenderGraphPass<RejectionPass, RejectionFrame, RejectionBindings>
		{
		public:
			RejectionPass(std::shared_ptr<PassContext> a_context, std::shared_ptr<org::Buffer> a_heartbeat) :
				_context(std::move(a_context)), _heartbeat(std::move(a_heartbeat)) {}

			RejectionBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				a_builder.UnorderedAccess(_heartbeat);
				return {};
			}

			RejectionFrame Prepare(const RejectionBindings&, const org::PassPrepareContext&) const
			{
				return RejectionFrame{ .work = _context->current, .context = *_context };
			}

			static void Record(const RejectionBindings&, const RejectionFrame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.work) {
					return;
				}
				const auto& work = *a_frame.work;
				const auto& context = a_frame.context;
				auto& commands = a_recording.Commands();
				using Sync = rhi::ResourceSyncState;
				using Access = rhi::ResourceAccessType;

				// Earlier frames' copies and dispatches may still read/write what this frame touches.
				GlobalBarrier(commands, Sync::All, Access::Common, Sync::All, Access::Common);
				commands.SetDescriptorHeaps(context.heap, context.samplerHeap);

				if (!work.stagingCopies.empty()) {
					for (const auto& [destination, source] : work.stagingCopies) {
						commands.CopyBufferRegion(destination, 0, source.first, 0, source.second);
					}
					GlobalBarrier(commands, Sync::Copy, Access::CopyDest, Sync::BuildRaytracingAccelerationStructure | Sync::Raytracing,
						Access::ShaderResource);
				}
				if (!work.blasBuilds.empty()) {
					commands.BuildAccelerationStructures(work.blasBuilds.data(), static_cast<std::uint32_t>(work.blasBuilds.size()));
					GlobalBarrier(commands, Sync::BuildRaytracingAccelerationStructure, Access::RaytracingAccelerationStructureWrite,
						Sync::BuildRaytracingAccelerationStructure | Sync::Raytracing, Access::RaytracingAccelerationStructureRead);
				}
				if (work.tlasBuild) {
					commands.BuildAccelerationStructures(&*work.tlasBuild, 1);
					GlobalBarrier(commands, Sync::BuildRaytracingAccelerationStructure, Access::RaytracingAccelerationStructureWrite,
						Sync::Raytracing, Access::RaytracingAccelerationStructureRead);
				}
				if (work.frame && work.frame->queryCount > 0) {
					const auto& frame = *work.frame;
					commands.BindLayout(context.layout);
					commands.BindPipeline(context.pipeline);
					const bool debug = context.debugCandidate != 0xFFFFFFFFu && frame.debug;
					if (debug) {
						commands.CopyBufferRegion(frame.debug->GetHandle(), 0, frame.debugZero.resource->GetHandle(), 0, kDebugBytes);
						GlobalBarrier(commands, Sync::Copy, Access::CopyDest, Sync::Raytracing, Access::UnorderedAccess);
					}
					const std::uint32_t constants[kRootConstantCount]{ context.tlasSrv, frame.candidatesSrv, frame.outputUav,
						static_cast<std::uint32_t>(frame.queryCount), std::bit_cast<std::uint32_t>(context.segmentLength),
						debug ? context.debugCandidate : 0xFFFFFFFFu, frame.debugUav };
					commands.PushConstants(rhi::ShaderStage::All, 0, 0, 0, kRootConstantCount, constants);
					rhi::RayTracingDispatchDesc dispatch{};
					dispatch.rayGenerationShaderTable = context.rayGen;
					dispatch.missShaderTable = context.miss;
					dispatch.hitGroupTable = context.hit;
					dispatch.width = static_cast<std::uint32_t>(frame.queryCount);
					commands.TraceRays(dispatch);
					GlobalBarrier(commands, Sync::Raytracing, Access::UnorderedAccess, Sync::Copy, Access::CopySource);
					commands.CopyBufferRegion(frame.readback.resource->GetHandle(), 0, frame.output->GetHandle(), 0,
						frame.queryCount * sizeof(std::uint32_t));
					if (debug) {
						commands.CopyBufferRegion(frame.debugReadback.resource->GetHandle(), 0, frame.debug->GetHandle(), 0, kDebugBytes);
					}
				}
			}

		private:
			std::shared_ptr<PassContext> _context;
			std::shared_ptr<org::Buffer> _heartbeat;
		};

		class RejectionExtension final : public org::RenderGraph::IRenderGraphExtension
		{
		public:
			RejectionExtension(std::shared_ptr<PassContext> a_context, std::shared_ptr<org::Buffer> a_heartbeat) :
				_context(std::move(a_context)), _heartbeat(std::move(a_heartbeat)) {}

			void PrepareForBuild(org::RenderGraph& a_graph) override
			{
				a_graph.RegisterResource(org::ResourceIdentifier("fasterngio.heartbeat"), _heartbeat);
			}

			void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) override
			{
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("fasterngio.reject",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<RejectionPass>(_context, _heartbeat)))
						.PreferQueue(org::QueueKind::Graphics));
			}

		private:
			std::shared_ptr<PassContext> _context;
			std::shared_ptr<org::Buffer> _heartbeat;
		};
	}

	class GpuRejector::Impl
	{
	public:
		explicit Impl(GpuRejectorDesc a_desc) :
			_desc(std::move(a_desc))
		{
			// Startup is the only time a caller waits on the render thread.
			std::promise<void> ready;
			auto started = ready.get_future();
			_thread = std::thread([this, &ready] {
				try {
					Initialize();
					ready.set_value();
				} catch (...) {
					ready.set_exception(std::current_exception());
					return;
				}
				Loop();
				Shutdown();
			});
			try {
				started.get();
			} catch (...) {
				_thread.join();
				throw;
			}
		}

		~Impl()
		{
			_stopping.store(true, std::memory_order_release);
			Wake();
			if (_thread.joinable()) {
				_thread.join();
			}
		}

		void PostWorld(std::shared_ptr<const Rejection::WorldIndex> a_world)
		{
			_inbox.Push(Command{ std::move(a_world) });
			Wake();
		}

		void Post(std::shared_ptr<TraceJob> a_job)
		{
			_inbox.Push(Command{ std::move(a_job) });
			Wake();
		}

		[[nodiscard]] GpuRejectorStats Stats() const
		{
			GpuRejectorStats stats;
			stats.adapter = _adapterName;
			stats.models = _statModels.load(std::memory_order_relaxed);
			stats.instances = _statInstances.load(std::memory_order_relaxed);
			stats.blasBytes = _statBlasBytes.load(std::memory_order_relaxed);
			stats.modelBytes = _statModelBytes.load(std::memory_order_relaxed);
			stats.framesExecuted = _statFrames.load(std::memory_order_relaxed);
			stats.queries = _statQueries.load(std::memory_order_relaxed);
			stats.worldBuildSeconds = _statWorldSeconds.load(std::memory_order_relaxed);
			return stats;
		}

	private:
		using Command = std::variant<std::shared_ptr<TraceJob>, std::shared_ptr<const Rejection::WorldIndex>>;

		void Wake()
		{
			_signal.fetch_add(1, std::memory_order_release);
			_signal.notify_one();
		}

		// ---- Render thread ------------------------------------------------------------------

		void Initialize()
		{
			CreateDeviceAndCheckFeatures();

			rhi::DescriptorHeapDesc heapDesc{ rhi::DescriptorHeapType::CbvSrvUav, kDescriptorCapacity, true, "FasterNGIO descriptors" };
			Check(_device->CreateDescriptorHeap(heapDesc, _heap), "descriptor heap");
			// Bindless root signatures index the sampler heap too, so one must be bound.
			rhi::DescriptorHeapDesc samplerDesc{ rhi::DescriptorHeapType::Sampler, 16, true, "FasterNGIO samplers" };
			Check(_device->CreateDescriptorHeap(samplerDesc, _samplerHeap), "sampler heap");

			CreatePipeline();

			_host = std::make_unique<org::PersistentGraphHost>(org::PersistentGraphHost::Desc{
				.device = _device.Get(),
				.backend = _backend,
				.tasks = std::make_shared<org::runtime::ThreadPoolTaskService>(2),
				.framesInFlight = 3,
				.epochOrder = {},
			});
			_heartbeat = org::Buffer::CreateSharedUnmaterialized(rhi::HeapType::DeviceLocal, 256, true);
			org::BufferBase::DescriptorRequirements requirements{};
			requirements.createUAV = true;
			requirements.uavDesc = { .dimension = rhi::UavDim::Buffer,
				.formatOverride = rhi::Format::R32_Typeless,
				.buffer = { .kind = rhi::BufferViewKind::Raw, .firstElement = 0, .numElements = 64 } };
			_heartbeat->SetDescriptorRequirements(requirements);
			_heartbeat->SetName("fasterngio.heartbeat");
			_passContext = std::make_shared<PassContext>();
			_passContext->heap = _heap->GetHandle();
			_passContext->samplerHeap = _samplerHeap->GetHandle();
			_passContext->layout = _layout->GetHandle();
			_passContext->pipeline = _pipeline->GetHandle();
			_passContext->rayGen = { _shaderTable.resource->GetHandle(), _rayGenOffset, _recordStride, _recordStride };
			_passContext->miss = { _shaderTable.resource->GetHandle(), _missOffset, _recordStride, _recordStride };
			_passContext->hit = { _shaderTable.resource->GetHandle(), _hitOffset, _recordStride * _hitCount, _recordStride };
			_passContext->segmentLength = _desc.segmentLength;
			if (const char* debugCandidate = std::getenv("FASTERNGIO_DEBUG_CANDIDATE")) {
				_passContext->debugCandidate = static_cast<std::uint32_t>(std::strtoul(debugCandidate, nullptr, 10));
			}
			auto context = _passContext;
			auto heartbeat = _heartbeat;
			_host->AddExtension("fasterngio", [context, heartbeat] { return std::make_unique<RejectionExtension>(context, heartbeat); });
			_host->SetCompletedFrameCallback([this](std::uint64_t a_frame, const org::runtime::IStatisticsService&) { OnFrameCompleted(a_frame); });
		}

		void Shutdown()
		{
			if (_device) {
				_device->WaitIdle();
			}
			_inFlight.clear();
			// ORG resources (the heartbeat buffer) must be gone before the host's destructor shuts
			// the runtime device down.
			if (_host) {
				_host->RemoveExtension("fasterngio");
				_host->DestroyGraph();
			}
			_heartbeat.reset();
			_passContext.reset();
			_frames.clear();
			_models.clear();
			_tlas.Reset();
			_tlasStorage.Reset();
			_tlasScratch.Reset();
			_tlasInstances = {};
			_blasScratch.Reset();
			_shaderTable = {};
			_pipeline.Reset();
			_layout.Reset();
			_heap.Reset();
			_samplerHeap.Reset();
			_host.reset();
			_device.Reset();
		}

		// Everything the ray-tracing path needs, checked up front so an unsuitable adapter falls back
		// to the CPU before any work is posted rather than failing mid-run.
		void CreateDeviceAndCheckFeatures()
		{
			_backend = _desc.api == GpuApi::D3D12 ? rhi::Backend::D3D12 : rhi::Backend::Vulkan;
			rhi::DeviceCreateInfo create{};
			create.backend = _backend;
			create.framesInFlight = 3;
			create.enableDebug = _desc.debugLayer;
			rhi::Result created = rhi::Result::Unsupported;
#if defined(_WIN32)
			if (_desc.api == GpuApi::D3D12) {
				created = rhi::CreateD3D12Device(create, _device);
			}
#endif
			if (_desc.api == GpuApi::Vulkan) {
				created = rhi::CreateVulkanDevice(create, _device);
			}
			if (!rhi::IsOk(created) || !_device) {
				throw GpuUnsupportedError(std::string("no ") + GpuApiName(_desc.api) + " device could be created");
			}

			AdapterFeatureInfo adapter{};
			ShaderFeatureInfo shader{};
			RayTracingFeatureInfo rayTracing{};
			ResourceAllocationFeatureInfo allocation{};
			adapter.header.pNext = &shader.header;
			shader.header.pNext = &rayTracing.header;
			rayTracing.header.pNext = &allocation.header;
			Check(_device->QueryFeatureInfo(&adapter.header), "feature query");
			_adapterName = adapter.name;

			const bool vulkan = _desc.api == GpuApi::Vulkan;
			std::vector<std::string> missing;
			if (shader.maxShaderModel < ShaderModel::SM_6_6) {
				missing.emplace_back("shader model 6.6");
			}
			if (!shader.unifiedResourceHeaps || !shader.unboundedDescriptorTables) {
				missing.emplace_back(vulkan ? "bindless descriptor heaps (VK_EXT_descriptor_heap)" : "bindless descriptor heaps (resource binding tier 3)");
			}
			if (!rayTracing.pipeline) {
				missing.emplace_back(vulkan ? "ray-tracing pipelines (VK_KHR_ray_tracing_pipeline)" : "ray-tracing pipelines (DXR 1.0)");
			}
			if (!rayTracing.accelerationStructure) {
				missing.emplace_back(vulkan ? "acceleration structures (VK_KHR_acceleration_structure)" : "acceleration structures");
			}
			if (rayTracing.pipeline && rayTracing.maxRayRecursionDepth < 1) {
				missing.emplace_back("ray recursion depth 1");
			}
			if (rayTracing.pipeline && rayTracing.maxRayDispatchWidth < 65536) {
				missing.emplace_back("ray dispatches of 65536 launches");
			}
			if (!missing.empty()) {
				std::string list;
				for (const auto& item : missing) {
					list += (list.empty() ? "" : ", ") + item;
				}
				_device.Reset();
				throw GpuUnsupportedError(_adapterName + " (" + GpuApiName(_desc.api) + ") lacks " + list);
			}
			_rayTracing = rayTracing;
			_maxQueriesPerFrame = (std::min)(_desc.maxQueriesPerFrame, rayTracing.maxRayDispatchWidth);
			_gpuUploadHeap = allocation.gpuUploadHeapSupported && !std::getenv("FASTERNGIO_NO_GPU_UPLOAD_HEAP");
			constexpr const char* kTierNames[] = { "none", "DXR 1.0", "DXR 1.1", "VK_KHR_ray_tracing_pipeline", "DXR 2.0" };
			const auto tier = static_cast<std::size_t>(rayTracing.backendTier);
			spdlog::info("GPU: {} ({}), {}, GPU upload heap {}", _adapterName, GpuApiName(_desc.api), tier < std::size(kTierNames) ? kTierNames[tier] : "unknown tier",
				_gpuUploadHeap ? "yes" : "no");
		}

		void CreatePipeline()
		{
			const auto shaderPath = _desc.shaderDirectory / "GrassRejection.hlsl";
			std::ifstream file(shaderPath, std::ios::binary);
			if (!file) {
				throw std::runtime_error("GPU: cannot read " + shaderPath.string());
			}
			const std::vector<char> source{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
			org::services::ShaderCompiler compiler(_desc.shaderCacheDirectory);
			if (!compiler.Available()) {
				throw GpuUnsupportedError("the DXC shader compiler (dxcompiler) is not available");
			}
			org::services::ShaderCompileRequest request{};
			request.sourceName = shaderPath.string();
			request.source = std::as_bytes(std::span(source));
			request.target = L"lib_6_6";
			request.format = _desc.api == GpuApi::Vulkan ? org::services::ShaderBinaryFormat::Spirv : org::services::ShaderBinaryFormat::Dxil;
			request.includeDirectories = { _desc.shaderDirectory };
			request.dependencyFiles = { _desc.shaderDirectory / "Shared" / "GrassQueryMath.hlsli" };
			request.defines = {
				{ L"QUERY_RAY", _desc.mode == Rejection::QueryMode::Ray ? L"1" : L"0" },
				{ L"DEBUG_QUERIES", std::getenv("FASTERNGIO_DEBUG_CANDIDATE") ? L"1" : L"0" },
			};
			const auto artifact = compiler.Compile(std::move(request));
			if (!artifact) {
				throw std::runtime_error("GPU: shader compilation failed:\n" + artifact.diagnostics);
			}
			_shaderBinary = artifact.binary;

			rhi::PushConstantRangeDesc constants{};
			constants.visibility = rhi::ShaderStage::All;
			constants.num32BitValues = kRootConstantCount;
			Check(_device->CreatePipelineLayout(rhi::PipelineLayoutDesc{ .pushConstants = { &constants, 1 }, .flags = rhi::PF_None }, _layout),
				"pipeline layout");

			const rhi::ShaderBinary binary{ _shaderBinary.data(), static_cast<std::uint32_t>(_shaderBinary.size()) };
			rhi::SubobjShader shaders[] = {
				{ rhi::ShaderStage::RayGen, binary, "GrassRayGen" },
				{ rhi::ShaderStage::Miss, binary, "GrassMiss" },
				{ rhi::ShaderStage::ClosestHit, binary, "GrassClosestHit" },
				{ rhi::ShaderStage::Intersection, binary, "TriangleIntersection" },
				{ rhi::ShaderStage::Intersection, binary, "HullIntersection" },
				{ rhi::ShaderStage::Intersection, binary, "CapsuleIntersection" },
			};
			rhi::RayTracingShaderGroupDesc groups[] = {
				{ .type = rhi::RayTracingShaderGroupType::General, .name = "GrassRayGen", .generalShader = "GrassRayGen" },
				{ .type = rhi::RayTracingShaderGroupType::General, .name = "GrassMiss", .generalShader = "GrassMiss" },
				{ .type = rhi::RayTracingShaderGroupType::ProceduralHitGroup, .name = "TriangleHitGroup", .closestHitShader = "GrassClosestHit", .intersectionShader = "TriangleIntersection" },
				{ .type = rhi::RayTracingShaderGroupType::ProceduralHitGroup, .name = "HullHitGroup", .closestHitShader = "GrassClosestHit", .intersectionShader = "HullIntersection" },
				{ .type = rhi::RayTracingShaderGroupType::ProceduralHitGroup, .name = "CapsuleHitGroup", .closestHitShader = "GrassClosestHit", .intersectionShader = "CapsuleIntersection" },
			};
			rhi::SubobjRayTracingPipeline pipeline{};
			pipeline.globalLayout = _layout->GetHandle();
			pipeline.shaders = { shaders, static_cast<std::uint32_t>(std::size(shaders)) };
			pipeline.shaderGroups = { groups, static_cast<std::uint32_t>(std::size(groups)) };
			pipeline.shaderConfig.maxPayloadSizeInBytes = 4;
			pipeline.shaderConfig.maxAttributeSizeInBytes = 4;
			pipeline.pipelineConfig.maxTraceRecursionDepth = 1;
			const rhi::PipelineStreamItem items[] = { rhi::Make(pipeline) };
			Check(_device->CreatePipeline(items, 1, _pipeline), "ray-tracing pipeline");

			// Shader table: raygen, miss, then hit records for every non-empty subset of the three
			// kinds, each subset's records in kind order (matching the BLAS geometry order).
			const auto handleSize = _rayTracing.shaderGroupHandleSize;
			std::vector<std::byte> handles(static_cast<std::size_t>(handleSize) * std::size(groups));
			Check(_device->GetRayTracingShaderGroupHandles(_pipeline->GetHandle(), 0, static_cast<std::uint32_t>(std::size(groups)), handles.data(),
					  static_cast<std::uint32_t>(handles.size())),
				"shader group handles");
			_recordStride = rhi::AlignRayTracingShaderRecordSize(handleSize, _rayTracing.shaderTableStrideAlignment);
			const auto baseAlignment = _rayTracing.shaderGroupBaseAlignment;
			_rayGenOffset = 0;
			_missOffset = AlignUp(_rayGenOffset + _recordStride, baseAlignment);
			_hitOffset = AlignUp(_missOffset + _recordStride, baseAlignment);
			std::vector<std::uint32_t> hitRecords;
			for (std::uint32_t mask = 1; mask < (1u << KindCount); ++mask) {
				_subsetBase[mask] = static_cast<std::uint32_t>(hitRecords.size());
				for (std::uint32_t kind = 0; kind < KindCount; ++kind) {
					if (mask & (1u << kind)) {
						hitRecords.push_back(2 + kind);
					}
				}
			}
			_hitCount = static_cast<std::uint32_t>(hitRecords.size());
			const auto tableBytes = _hitOffset + _recordStride * _hitCount;
			_shaderTable = CreateMapped(tableBytes, rhi::HeapType::Upload, "FasterNGIO shader table");
			const auto writeRecord = [&](std::uint64_t a_offset, std::uint32_t a_group) {
				rhi::WriteRayTracingShaderRecord(_shaderTable.mapped + a_offset, _recordStride, handles.data() + a_group * handleSize, handleSize);
			};
			writeRecord(_rayGenOffset, 0);
			writeRecord(_missOffset, 1);
			for (std::uint32_t i = 0; i < _hitCount; ++i) {
				writeRecord(_hitOffset + i * _recordStride, hitRecords[i]);
			}
		}

		[[nodiscard]] MappedBuffer CreateMapped(std::uint64_t a_bytes, rhi::HeapType a_heap, const char* a_name)
		{
			MappedBuffer buffer;
			buffer.bytes = (std::max<std::uint64_t>)(a_bytes, 256);
			Check(_device->CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(buffer.bytes, a_heap, rhi::RF_None, a_name), buffer.resource),
				a_name);
			void* mapped = nullptr;
			buffer.resource->Map(&mapped, 0, a_heap == rhi::HeapType::Readback ? buffer.bytes : 0);
			buffer.mapped = static_cast<std::byte*>(mapped);
			return buffer;
		}

		[[nodiscard]] std::uint32_t AllocateDescriptor()
		{
			if (!_freeDescriptors.empty()) {
				const auto slot = _freeDescriptors.back();
				_freeDescriptors.pop_back();
				return slot;
			}
			if (_nextDescriptor >= kDescriptorCapacity) {
				throw std::runtime_error("GPU: descriptor heap exhausted");
			}
			return _nextDescriptor++;
		}

		void WriteRawSrv(std::uint32_t a_slot, const rhi::ResourcePtr& a_resource, std::uint64_t a_bytes)
		{
			rhi::SrvDesc srv{};
			srv.dimension = rhi::SrvDim::Buffer;
			srv.formatOverride = rhi::Format::R32_Typeless;
			srv.buffer.kind = rhi::BufferViewKind::Raw;
			srv.buffer.numElements = static_cast<std::uint32_t>(a_bytes / 4);
			Check(_device->CreateShaderResourceView({ _heap->GetHandle(), a_slot }, a_resource->GetHandle(), srv), "raw SRV");
		}

		void WriteStructuredSrv(std::uint32_t a_slot, const rhi::ResourcePtr& a_resource, std::uint32_t a_count, std::uint32_t a_stride)
		{
			rhi::SrvDesc srv{};
			srv.dimension = rhi::SrvDim::Buffer;
			srv.buffer.kind = rhi::BufferViewKind::Structured;
			srv.buffer.numElements = a_count;
			srv.buffer.structureByteStride = a_stride;
			Check(_device->CreateShaderResourceView({ _heap->GetHandle(), a_slot }, a_resource->GetHandle(), srv), "structured SRV");
		}

		void BuildWorldOnRenderThread(const Rejection::WorldIndex& a_world)
		{
			const auto begin = std::chrono::steady_clock::now();
			const auto& models = a_world.Models();
			const auto& instances = a_world.Instances();

			// Grow each model's AABBs by the widest query radius at its smallest placed scale, so the
			// AABBs contain every capsule that could touch the model at any of its references.
			std::vector<float> minScale(models.size(), (std::numeric_limits<float>::max)());
			for (const auto& instance : instances) {
				minScale[instance.model] = (std::min)(minScale[instance.model], instance.worldFromModel.scale);
			}

			_models.clear();
			_models.resize(models.size());
			std::vector<std::uint32_t> buildOrder;
			std::uint64_t modelBytes = 0;
			for (std::uint32_t i = 0; i < models.size(); ++i) {
				if (models[i].collision.status != Collision::ExtractionStatus::HasCollision || minScale[i] == (std::numeric_limits<float>::max)()) {
					continue;
				}
				const auto inflation = _desc.maxQueryRadius / (std::max)(minScale[i], 1.0e-3f);
				const auto packed = PackModel(models[i].collision, inflation);
				auto& model = _models[i];
				model.dataBytes = AlignUp(packed.bytes.size(), 16);
				model.kindMask = packed.kindMask;
				if (_gpuUploadHeap) {
					auto buffer = CreateMapped(model.dataBytes, rhi::HeapType::GPUUpload, "FasterNGIO model");
					std::memcpy(buffer.mapped, packed.bytes.data(), packed.bytes.size());
					model.data = std::move(buffer.resource);
				} else {
					auto staging = CreateMapped(model.dataBytes, rhi::HeapType::Upload, "FasterNGIO model staging");
					std::memcpy(staging.mapped, packed.bytes.data(), packed.bytes.size());
					model.staging = std::move(staging.resource);
					Check(_device->CreateCommittedResource(
							  rhi::helpers::ResourceDesc::Buffer(model.dataBytes, rhi::HeapType::DeviceLocal, rhi::RF_None, "FasterNGIO model"), model.data),
						"model buffer");
				}
				modelBytes += model.dataBytes;
				model.srv = AllocateDescriptor();
				WriteRawSrv(model.srv, model.data, model.dataBytes);

				for (const auto& geometry : packed.geometries) {
					rhi::RayTracingGeometryDesc desc{};
					desc.type = rhi::RayTracingGeometryType::Aabbs;
					desc.flags = rhi::RTGeometry_Opaque;
					desc.aabbs.aabbBuffer = { model.data->GetHandle(), geometry.aabbOffset, static_cast<std::uint64_t>(geometry.count) * sizeof(Aabb) };
					desc.aabbs.stride = sizeof(Aabb);
					desc.aabbs.count = geometry.count;
					model.geometries.push_back(desc);
				}
				rhi::AccelerationStructureBuildInputs inputs{};
				inputs.type = rhi::RayTracingAccelerationStructureType::BottomLevel;
				inputs.flags = rhi::RTASBuild_PreferFastTrace;
				inputs.geometries = { model.geometries.data(), static_cast<std::uint32_t>(model.geometries.size()) };
				Check(_device->GetAccelerationStructurePrebuildInfo(inputs, model.prebuild), "BLAS prebuild info");
				const auto storageBytes = AlignUp(model.prebuild.resultDataMaxSizeInBytes, kAccelerationStructureAlignment);
				Check(_device->CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(storageBytes, rhi::HeapType::DeviceLocal,
														   static_cast<rhi::ResourceFlags>(rhi::RF_RaytracingAccelerationStructure | rhi::RF_AllowUnorderedAccess),
														   "FasterNGIO BLAS"),
						  model.blasStorage),
					"BLAS storage");
				rhi::AccelerationStructureDesc blasDesc{};
				blasDesc.type = rhi::RayTracingAccelerationStructureType::BottomLevel;
				blasDesc.storage = model.blasStorage->GetHandle();
				blasDesc.sizeBytes = storageBytes;
				blasDesc.flags = rhi::RTASBuild_PreferFastTrace;
				Check(_device->CreateAccelerationStructure(blasDesc, model.blas), "BLAS");
				model.blasAddress = _device->GetAccelerationStructureDeviceAddress(model.blas->GetHandle());
				buildOrder.push_back(i);
				_statBlasBytes.fetch_add(storageBytes, std::memory_order_relaxed);
			}

			// Build the BLASes in frames bounded by a scratch budget.
			std::size_t next = 0;
			while (next < buildOrder.size()) {
				auto work = std::make_shared<FrameWork>();
				std::uint64_t scratchBytes = 0;
				const auto first = next;
				while (next < buildOrder.size()) {
					const auto bytes = AlignUp(_models[buildOrder[next]].prebuild.scratchDataSizeInBytes, kAccelerationStructureAlignment);
					if (next > first && scratchBytes + bytes > kBlasScratchBudget) {
						break;
					}
					scratchBytes += bytes;
					++next;
				}
				EnsureScratch(_blasScratch, _blasScratchBytes, scratchBytes, "FasterNGIO BLAS scratch");
				std::uint64_t scratchOffset = 0;
				for (auto i = first; i < next; ++i) {
					auto& model = _models[buildOrder[i]];
					if (model.staging) {
						work->stagingCopies.push_back({ model.data->GetHandle(), { model.staging->GetHandle(), model.dataBytes } });
					}
					rhi::AccelerationStructureBuildDesc build{};
					build.mode = rhi::RayTracingAccelerationStructureBuildMode::Build;
					build.destination = model.blas->GetHandle();
					build.inputs.type = rhi::RayTracingAccelerationStructureType::BottomLevel;
					build.inputs.flags = rhi::RTASBuild_PreferFastTrace;
					build.inputs.geometries = { model.geometries.data(), static_cast<std::uint32_t>(model.geometries.size()) };
					const auto bytes = AlignUp(model.prebuild.scratchDataSizeInBytes, kAccelerationStructureAlignment);
					build.scratch = { _blasScratch->GetHandle(), scratchOffset, bytes };
					scratchOffset += bytes;
					work->blasBuilds.push_back(build);
				}
				ExecuteFrame(std::move(work), nullptr);
				// The scratch buffer is reused by the next chunk.
				_device->WaitIdle();
			}

			// World TLAS: one instance per placed reference.
			std::vector<rhi::PackedRayTracingInstanceDesc> packed;
			packed.reserve(instances.size());
			for (const auto& instance : instances) {
				const auto& model = _models[instance.model];
				if (!model.blas) {
					continue;
				}
				rhi::RayTracingInstanceDesc desc{};
				std::memcpy(desc.transform, instance.worldFromModel.m, sizeof(desc.transform));
				desc.instanceID = model.srv;
				desc.instanceMask = 0xFF;
				desc.instanceContributionToHitGroupIndex = _subsetBase[model.kindMask];
				desc.flags = rhi::RTInstance_ForceOpaque;
				packed.push_back(rhi::PackRayTracingInstanceDesc(desc, model.blasAddress));
			}
			_tlasInstances = CreateMapped(packed.size() * sizeof(rhi::PackedRayTracingInstanceDesc), rhi::HeapType::Upload, "FasterNGIO TLAS instances");
			std::memcpy(_tlasInstances.mapped, packed.data(), packed.size() * sizeof(rhi::PackedRayTracingInstanceDesc));
			_tlasGeometry = {};
			_tlasGeometry.type = rhi::RayTracingGeometryType::Instances;
			_tlasGeometry.instances.instanceBuffer = { _tlasInstances.resource->GetHandle(), 0, packed.size() * sizeof(rhi::PackedRayTracingInstanceDesc) };
			_tlasGeometry.instances.stride = sizeof(rhi::PackedRayTracingInstanceDesc);
			_tlasGeometry.instances.count = static_cast<std::uint32_t>(packed.size());
			rhi::AccelerationStructureBuildInputs inputs{};
			inputs.type = rhi::RayTracingAccelerationStructureType::TopLevel;
			inputs.flags = rhi::RTASBuild_PreferFastTrace;
			inputs.geometries = { &_tlasGeometry, 1 };
			rhi::AccelerationStructurePrebuildInfo prebuild{};
			Check(_device->GetAccelerationStructurePrebuildInfo(inputs, prebuild), "TLAS prebuild info");
			const auto tlasBytes = AlignUp(prebuild.resultDataMaxSizeInBytes, kAccelerationStructureAlignment);
			Check(_device->CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(tlasBytes, rhi::HeapType::DeviceLocal,
													   static_cast<rhi::ResourceFlags>(rhi::RF_RaytracingAccelerationStructure | rhi::RF_AllowUnorderedAccess),
													   "FasterNGIO TLAS"),
					  _tlasStorage),
				"TLAS storage");
			rhi::AccelerationStructureDesc tlasDesc{};
			tlasDesc.type = rhi::RayTracingAccelerationStructureType::TopLevel;
			tlasDesc.storage = _tlasStorage->GetHandle();
			tlasDesc.sizeBytes = tlasBytes;
			tlasDesc.flags = rhi::RTASBuild_PreferFastTrace;
			Check(_device->CreateAccelerationStructure(tlasDesc, _tlas), "TLAS");
			EnsureScratch(_tlasScratch, _tlasScratchBytes, AlignUp(prebuild.scratchDataSizeInBytes, kAccelerationStructureAlignment), "FasterNGIO TLAS scratch");
			auto work = std::make_shared<FrameWork>();
			rhi::AccelerationStructureBuildDesc build{};
			build.mode = rhi::RayTracingAccelerationStructureBuildMode::Build;
			build.destination = _tlas->GetHandle();
			build.inputs = inputs;
			build.scratch = { _tlasScratch->GetHandle(), 0, _tlasScratchBytes };
			work->tlasBuild = build;
			ExecuteFrame(std::move(work), nullptr);
			_device->WaitIdle();

			_tlasSrv = AllocateDescriptor();
			_passContext->tlasSrv = _tlasSrv;
			rhi::SrvDesc srv{};
			srv.dimension = rhi::SrvDim::AccelerationStruct;
			srv.accel.accelerationStructure = _tlas->GetHandle();
			srv.accel.sizeBytes = tlasBytes;
			// The view addresses the TLAS itself; BasicRHI treats an empty resource as a null view.
			Check(_device->CreateShaderResourceView({ _heap->GetHandle(), _tlasSrv }, _tlasStorage->GetHandle(), srv), "TLAS SRV");

			// Staging copies are done; the device-local copies stay.
			for (auto& model : _models) {
				model.staging.Reset();
			}
			_worldReady = true;
			_statModels.store(buildOrder.size(), std::memory_order_relaxed);
			_statInstances.store(packed.size(), std::memory_order_relaxed);
			_statModelBytes.store(modelBytes, std::memory_order_relaxed);
			_statWorldSeconds.store(std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count(), std::memory_order_relaxed);
		}

		void EnsureScratch(rhi::ResourcePtr& a_buffer, std::uint64_t& a_capacity, std::uint64_t a_bytes, const char* a_name)
		{
			if (a_buffer && a_capacity >= a_bytes) {
				return;
			}
			a_buffer.Reset();
			a_capacity = AlignUp((std::max<std::uint64_t>)(a_bytes, 1ull << 20), kAccelerationStructureAlignment);
			Check(_device->CreateCommittedResource(
					  rhi::helpers::ResourceDesc::Buffer(a_capacity, rhi::HeapType::DeviceLocal, rhi::RF_AllowUnorderedAccess, a_name), a_buffer),
				a_name);
		}

		FrameSet& AcquireFrameSet()
		{
			for (auto& frame : _frames) {
				if (!frame->busy) {
					return *frame;
				}
			}
			auto frame = std::make_unique<FrameSet>();
			frame->candidatesSrv = AllocateDescriptor();
			frame->outputUav = AllocateDescriptor();
			if (_passContext->debugCandidate != 0xFFFFFFFFu) {
				Check(_device->CreateCommittedResource(
						  rhi::helpers::ResourceDesc::Buffer(kDebugBytes, rhi::HeapType::DeviceLocal, rhi::RF_AllowUnorderedAccess, "FasterNGIO debug"), frame->debug),
					"debug buffer");
				frame->debugZero = CreateMapped(kDebugBytes, rhi::HeapType::Upload, "FasterNGIO debug zero");
				std::memset(frame->debugZero.mapped, 0, kDebugBytes);
				frame->debugReadback = CreateMapped(kDebugBytes, rhi::HeapType::Readback, "FasterNGIO debug readback");
				frame->debugUav = AllocateDescriptor();
				rhi::UavDesc uav{};
				uav.dimension = rhi::UavDim::Buffer;
				uav.formatOverride = rhi::Format::R32_Typeless;
				uav.buffer.kind = rhi::BufferViewKind::Raw;
				uav.buffer.numElements = static_cast<std::uint32_t>(kDebugBytes / 4);
				Check(_device->CreateUnorderedAccessView({ _heap->GetHandle(), frame->debugUav }, frame->debug->GetHandle(), uav), "debug UAV");
			}
			_frames.push_back(std::move(frame));
			return *_frames.back();
		}

		// Sizes a frame set for its queries and concatenates the jobs' candidates.
		void FillFrameSet(FrameSet& a_frame, std::vector<std::shared_ptr<TraceJob>>& a_jobs)
		{
			std::uint64_t queries = 0;
			for (const auto& job : a_jobs) {
				queries += job->Queries().size();
			}

			if (a_frame.candidates.bytes < queries * sizeof(Query)) {
				a_frame.candidates = CreateMapped(queries * sizeof(Query) * 3 / 2, rhi::HeapType::Upload, "FasterNGIO candidates");
				WriteStructuredSrv(a_frame.candidatesSrv, a_frame.candidates.resource, static_cast<std::uint32_t>(a_frame.candidates.bytes / sizeof(Query)), sizeof(Query));
			}
			if (a_frame.outputCapacity < queries) {
				a_frame.outputCapacity = queries * 3 / 2;
				const auto bytes = a_frame.outputCapacity * sizeof(std::uint32_t);
				a_frame.output.Reset();
				Check(_device->CreateCommittedResource(
						  rhi::helpers::ResourceDesc::Buffer(bytes, rhi::HeapType::DeviceLocal, rhi::RF_AllowUnorderedAccess, "FasterNGIO output"),
						  a_frame.output),
					"output buffer");
				rhi::UavDesc uav{};
				uav.dimension = rhi::UavDim::Buffer;
				uav.buffer.kind = rhi::BufferViewKind::Structured;
				uav.buffer.numElements = static_cast<std::uint32_t>(a_frame.outputCapacity);
				uav.buffer.structureByteStride = sizeof(std::uint32_t);
				Check(_device->CreateUnorderedAccessView({ _heap->GetHandle(), a_frame.outputUav }, a_frame.output->GetHandle(), uav), "output UAV");
				a_frame.readback = CreateMapped(bytes, rhi::HeapType::Readback, "FasterNGIO readback");
			}

			auto* candidates = reinterpret_cast<Query*>(a_frame.candidates.mapped);
			std::uint64_t offset = 0;
			a_frame.pending.clear();
			for (auto& job : a_jobs) {
				const auto count = job->Queries().size();
				std::memcpy(candidates + offset, job->Queries().data(), count * sizeof(Query));
				job->ReleaseQueries();
				a_frame.pending.push_back({ std::move(job), offset, count });
				offset += count;
			}
			a_frame.queryCount = queries;
		}

		void ExecuteFrame(std::shared_ptr<FrameWork> a_work, FrameSet* a_frame)
		{
			a_work->frame = a_frame;
			_passContext->current = std::move(a_work);
			_host->ExecuteFrame();
			_passContext->current.reset();
			if (a_frame) {
				a_frame->busy = true;
				_inFlight.emplace(_host->LastHostFrame(), a_frame);
			}
			_statFrames.fetch_add(1, std::memory_order_relaxed);
		}

		void OnFrameCompleted(std::uint64_t a_frameNumber)
		{
			const auto it = _inFlight.find(a_frameNumber);
			if (it == _inFlight.end()) {
				return;
			}
			CompleteFrame(*it->second);
			_inFlight.erase(it);
		}

		void CompleteFrame(FrameSet& a_frame)
		{
			if (a_frame.debug) {
				const auto* words = reinterpret_cast<const std::uint32_t*>(a_frame.debugReadback.mapped);
				const auto count = (std::min<std::uint32_t>)(words[0], 255);
				for (std::uint32_t i = 0; i < count; ++i) {
					const auto* record = words + 4 + i * 24;
					const auto* f = reinterpret_cast<const float*>(record);
					spdlog::info("debug stage={} instance={} primitive={} result={} p=({:.3f},{:.3f},{:.3f},{:.3f}) q=({:.3f},{:.3f},{:.3f},{:.3f}) a=({:.2f},{:.2f},{:.2f}) b=({:.3f},{:.3f},{:.3f},{:.3f}) c=({:.2f},{:.2f},{:.2f})",
						record[0], record[1], record[2], record[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[16], f[17], f[18], f[19], f[20], f[21], f[22]);
				}
			}
			const auto* hits = reinterpret_cast<const std::uint32_t*>(a_frame.readback.mapped);
			for (auto& pending : a_frame.pending) {
				pending.job->Finish(std::span(hits + pending.offset, pending.count), false);
			}
			_statQueries.fetch_add(a_frame.queryCount, std::memory_order_relaxed);
			a_frame.pending.clear();
			a_frame.busy = false;
		}

		void CompleteAllInFlight()
		{
			if (_inFlight.empty()) {
				return;
			}
			_device->WaitIdle();
			for (auto& [frameNumber, frame] : _inFlight) {
				CompleteFrame(*frame);
			}
			_inFlight.clear();
		}

		void Loop()
		{
			std::deque<std::shared_ptr<TraceJob>> waiting;
			try {
				while (true) {
					const auto seen = _signal.load(std::memory_order_acquire);
					while (auto command = _inbox.TryPop()) {
						if (auto* job = std::get_if<std::shared_ptr<TraceJob>>(&*command)) {
							waiting.push_back(std::move(*job));
						} else {
							BuildWorldOnRenderThread(*std::get<std::shared_ptr<const Rejection::WorldIndex>>(*command));
						}
					}
					if (_worldReady && !waiting.empty()) {
						std::vector<std::shared_ptr<TraceJob>> jobs;
						std::uint64_t queries = 0;
						while (!waiting.empty() && (jobs.empty() || queries + waiting.front()->Queries().size() <= _maxQueriesPerFrame)) {
							queries += waiting.front()->Queries().size();
							jobs.push_back(std::move(waiting.front()));
							waiting.pop_front();
						}
						auto& frame = AcquireFrameSet();
						FillFrameSet(frame, jobs);
						ExecuteFrame(std::make_shared<FrameWork>(), &frame);
						continue;
					}
					if (!_inFlight.empty()) {
						// Nothing new to trace: finish what is in flight instead of ticking empty frames.
						CompleteAllInFlight();
						continue;
					}
					if (_stopping.load(std::memory_order_acquire) && waiting.empty()) {
						return;
					}
					_signal.wait(seen, std::memory_order_acquire);
				}
			} catch (const std::exception& e) {
				spdlog::error("GPU: render thread failed: {}", e.what());
			} catch (...) {
				spdlog::error("GPU: render thread failed");
			}
			// Fail every job still owed a result so no consumer waits forever.
			for (auto& [frameNumber, frame] : _inFlight) {
				for (auto& pending : frame->pending) {
					pending.job->Finish({}, true);
				}
			}
			_inFlight.clear();
			for (auto& job : waiting) {
				job->Finish({}, true);
			}
			while (!_stopping.load(std::memory_order_acquire)) {
				while (auto command = _inbox.TryPop()) {
					if (auto* job = std::get_if<std::shared_ptr<TraceJob>>(&*command)) {
						(*job)->Finish({}, true);
					}
				}
				const auto seen = _signal.load(std::memory_order_acquire);
				if (_stopping.load(std::memory_order_acquire)) {
					break;
				}
				_signal.wait(seen, std::memory_order_acquire);
			}
		}

		GpuRejectorDesc _desc;
		std::thread _thread;
		Pipeline::MpscQueue<Command> _inbox;
		std::atomic<std::uint64_t> _signal{ 0 };
		std::atomic<bool> _stopping{ false };
		std::atomic<std::uint64_t> _statModels{ 0 };
		std::atomic<std::uint64_t> _statInstances{ 0 };
		std::atomic<std::uint64_t> _statBlasBytes{ 0 };
		std::atomic<std::uint64_t> _statModelBytes{ 0 };
		std::atomic<std::uint64_t> _statFrames{ 0 };
		std::atomic<std::uint64_t> _statQueries{ 0 };
		std::atomic<double> _statWorldSeconds{ 0.0 };

		// Render thread only.
		rhi::Backend _backend{ rhi::Backend::D3D12 };
		rhi::DevicePtr _device;
		// Written once by the render thread before the constructor returns.
		std::string _adapterName;
		RayTracingFeatureInfo _rayTracing{};
		std::uint32_t _maxQueriesPerFrame{ 0 };
		bool _gpuUploadHeap{ false };
		rhi::DescriptorHeapPtr _heap;
		rhi::DescriptorHeapPtr _samplerHeap;
		std::uint32_t _nextDescriptor{ 0 };
		std::vector<std::uint32_t> _freeDescriptors;
		std::vector<std::byte> _shaderBinary;
		rhi::PipelineLayoutPtr _layout;
		rhi::PipelinePtr _pipeline;
		MappedBuffer _shaderTable;
		std::uint64_t _recordStride{ 0 };
		std::uint64_t _rayGenOffset{ 0 };
		std::uint64_t _missOffset{ 0 };
		std::uint64_t _hitOffset{ 0 };
		std::uint32_t _hitCount{ 0 };
		std::array<std::uint32_t, 1u << KindCount> _subsetBase{};
		std::unique_ptr<org::PersistentGraphHost> _host;
		std::shared_ptr<org::Buffer> _heartbeat;
		std::shared_ptr<PassContext> _passContext;
		std::vector<GpuModel> _models;
		rhi::ResourcePtr _blasScratch;
		std::uint64_t _blasScratchBytes{ 0 };
		rhi::RayTracingGeometryDesc _tlasGeometry{};
		MappedBuffer _tlasInstances;
		rhi::ResourcePtr _tlasStorage;
		rhi::AccelerationStructurePtr _tlas;
		rhi::ResourcePtr _tlasScratch;
		std::uint64_t _tlasScratchBytes{ 0 };
		std::uint32_t _tlasSrv{ 0 };
		bool _worldReady{ false };
		std::vector<std::unique_ptr<FrameSet>> _frames;
		std::map<std::uint64_t, FrameSet*> _inFlight;
	};

	TraceJob::TraceJob(std::vector<Query> a_queries) :
		_queries(std::move(a_queries))
	{}

	TraceJob::~TraceJob()
	{
		auto* node = _subscribers.load(std::memory_order_acquire);
		if (node == ClosedList()) {
			return;
		}
		while (node) {
			delete std::exchange(node, node->next);
		}
	}

	bool TraceJob::Subscribe(std::function<void()> a_callback)
	{
		auto* node = new Subscriber{ std::move(a_callback) };
		auto* head = _subscribers.load(std::memory_order_acquire);
		do {
			if (head == ClosedList()) {
				delete node;
				return false;
			}
			node->next = head;
		} while (!_subscribers.compare_exchange_weak(head, node, std::memory_order_acq_rel, std::memory_order_acquire));
		return true;
	}

	void TraceJob::Finish(std::span<const std::uint32_t> a_hits, bool a_failed) noexcept
	{
		_hits.assign(a_hits.begin(), a_hits.end());
		_failed.store(a_failed, std::memory_order_release);
		_complete.store(true, std::memory_order_release);
		auto* node = _subscribers.exchange(ClosedList(), std::memory_order_acq_rel);
		while (node) {
			if (node->callback) {
				node->callback();
			}
			delete std::exchange(node, node->next);
		}
	}

	void TraceJob::ReleaseQueries() noexcept
	{
		std::vector<Query>().swap(_queries);
	}

	GpuRejector::GpuRejector(GpuRejectorDesc a_desc) :
		_impl(std::make_unique<Impl>(std::move(a_desc)))
	{}

	GpuRejector::~GpuRejector() = default;

	void GpuRejector::PostWorld(std::shared_ptr<const Rejection::WorldIndex> a_world)
	{
		_impl->PostWorld(std::move(a_world));
	}

	void GpuRejector::Post(std::shared_ptr<TraceJob> a_job)
	{
		_impl->Post(std::move(a_job));
	}

	GpuRejectorStats GpuRejector::Stats() const
	{
		return _impl->Stats();
	}
}
