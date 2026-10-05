#include "Diagnostics.h"

#include "WorldSetup.h"

#include "Collision/NifCollisionExtractor.h"
#include "Collision/ObjExport.h"
#include "Concurrency/AtomicWait.h"
#include "Rejection/CpuBvh.h"

#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/task_arena.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdlib>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <numeric>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace FasterNGIO::App
{
	namespace
	{
		// One model's collision, traced, as OBJ; and its render bounds and OBND for comparison.
		int DumpCollision(const GenerateOptions& a_options, const LoadedPlugins& a_plugins)
		{
			const auto resolver = MakeResolver(a_options, a_plugins);
			const auto modelPath = GameData::NormalizeModelPath(a_options.dumpCollisionModel);
			const auto bytes = resolver.Read(modelPath);
			if (!bytes) {
				throw std::runtime_error("model not found: " + modelPath);
			}
			const auto model = Collision::ExtractCollision(*bytes, Collision::ExtractionOptions{ .trace = true, .renderBounds = true });
			for (const auto& line : model.stats.trace) {
				spdlog::info("{}", line);
			}
			spdlog::info("{}: status={} triangles={} hulls={} hull_triangles={} capsules={} bodies_kept={} filtered={}", modelPath, static_cast<int>(model.status),
				model.triangles.size(), model.hulls.size(), model.hullTriangles.size(), model.capsules.size(), model.stats.bodiesKept, model.stats.bodiesFilteredByLayer);
			spdlog::info("collision aabb ({:.0f}, {:.0f}, {:.0f}) - ({:.0f}, {:.0f}, {:.0f})", model.aabbMin.x, model.aabbMin.y, model.aabbMin.z, model.aabbMax.x,
				model.aabbMax.y, model.aabbMax.z);
			spdlog::info("render aabb ({:.0f}, {:.0f}, {:.0f}) - ({:.0f}, {:.0f}, {:.0f})", model.renderAabbMin.x, model.renderAabbMin.y, model.renderAabbMin.z,
				model.renderAabbMax.x, model.renderAabbMax.y, model.renderAabbMax.z);
			for (const auto& [formID, base] : a_plugins.snapshot.baseObjectsByFormID) {
				if (base.bounds.present && GameData::NormalizeModelPath(base.modelPath) == modelPath) {
					const auto& b = base.bounds;
					spdlog::info("OBND {:08X}: ({}, {}, {}) - ({}, {}, {})", formID.value, b.min[0], b.min[1], b.min[2], b.max[0], b.max[1], b.max[2]);
				}
			}
			Collision::WriteObj(model, a_options.dumpCollisionPath);
			return 0;
		}

		// Extracts the collision of every model the selected cells reference and reports what was found.
		int CollisionSurvey(const GenerateOptions& a_options, const LoadedPlugins& a_plugins)
		{
			const auto& snapshot = a_plugins.snapshot;
			const auto resolver = MakeResolver(a_options, a_plugins);
			std::unordered_map<std::string, std::uint64_t> referencesByModel;
			std::uint64_t references = 0;
			for (const auto& [key, placements] : snapshot.exteriorPlacementsByCell) {
				if (key.worldFormID != a_options.worldFormID || !IsSelectedCell(a_options, key.x, key.y)) {
					continue;
				}
				for (const auto& placement : placements) {
					const auto baseIt = snapshot.baseObjectsByFormID.find(placement.baseFormID);
					if (baseIt == snapshot.baseObjectsByFormID.end() || baseIt->second.modelPath.empty()) {
						continue;
					}
					++referencesByModel[GameData::NormalizeModelPath(baseIt->second.modelPath)];
					++references;
				}
			}
			std::vector<std::pair<std::string, std::uint64_t>> models(referencesByModel.begin(), referencesByModel.end());
			spdlog::info("{} reference(s) to {} unique model(s)", references, models.size());

			const auto begin = std::chrono::steady_clock::now();
			std::vector<Collision::CollisionModel> results(models.size());
			std::vector<std::uint8_t> missing(models.size(), 0);
			oneapi::tbb::parallel_for(std::size_t{ 0 }, models.size(), [&](std::size_t i) {
				const auto bytes = resolver.Read(models[i].first);
				if (!bytes) {
					missing[i] = 1;
					results[i].status = Collision::ExtractionStatus::LoadFailed;
					return;
				}
				results[i] = Collision::ExtractCollision(*bytes, Collision::ExtractionOptions{ .renderBounds = true });
			});
			const auto elapsed = SecondsSince(begin);

			// Indexed by Collision::ExtractionStatus and Collision::ShapeType.
			constexpr std::array<const char*, 4> kStatusNames{ "has_collision", "filtered_by_layer", "no_collision", "load_failed" };
			constexpr std::array<const char*, static_cast<std::size_t>(Collision::ShapeType::Count)> kShapeNames{ "compressed_mesh", "packed_strips",
				"convex_vertices", "box", "sphere", "capsule", "multi_sphere", "list", "convex_list", "transform", "mopp", "unsupported" };
			std::array<std::uint64_t, kStatusNames.size()> modelsByStatus{};
			std::array<std::uint64_t, kStatusNames.size()> referencesByStatus{};
			std::array<std::uint64_t, kShapeNames.size()> shapes{};
			std::unordered_map<std::string, std::uint64_t> unsupported;
			std::uint64_t triangles = 0, hulls = 0, hullTriangles = 0, capsules = 0, missingCount = 0;
			std::vector<std::pair<std::uint64_t, std::string>> noCollision;
			for (std::size_t i = 0; i < models.size(); ++i) {
				const auto& model = results[i];
				const auto status = static_cast<std::size_t>(model.status);
				++modelsByStatus[status];
				referencesByStatus[status] += models[i].second;
				missingCount += missing[i];
				for (std::size_t s = 0; s < shapes.size(); ++s) {
					shapes[s] += model.stats.shapes[s];
				}
				for (const auto& name : model.stats.unsupportedShapes) {
					++unsupported[name];
				}
				triangles += model.triangles.size();
				hulls += model.hulls.size();
				hullTriangles += model.hullTriangles.size();
				capsules += model.capsules.size();
				if (model.status == Collision::ExtractionStatus::NoCollision) {
					noCollision.emplace_back(models[i].second, models[i].first);
				}
			}

			spdlog::info("extracted {} model(s) in {:.2f}s", models.size(), elapsed);
			for (std::size_t s = 0; s < kStatusNames.size(); ++s) {
				spdlog::info("  {:<18} models={:<6} references={}", kStatusNames[s], modelsByStatus[s], referencesByStatus[s]);
			}
			spdlog::info("  missing files: {}", missingCount);
			spdlog::info("  primitives: mesh_triangles={} hulls={} hull_triangles={} capsules={}", triangles, hulls, hullTriangles, capsules);
			for (std::size_t s = 0; s < shapes.size(); ++s) {
				if (shapes[s] != 0) {
					spdlog::info("  shape {:<16} {}", kShapeNames[s], shapes[s]);
				}
			}
			for (const auto& [name, count] : unsupported) {
				spdlog::info("  unsupported {} x{}", name, count);
			}
			// Collision should hug the render geometry of the same NIF (both use the same node chain).
			// A transform or Havok-scale mistake shows up as a systematic mismatch here.
			std::uint64_t boundsChecked = 0, boundsInside = 0;
			std::vector<std::pair<float, std::string>> boundsOutliers;
			for (std::size_t i = 0; i < models.size(); ++i) {
				const auto& model = results[i];
				if (model.status != Collision::ExtractionStatus::HasCollision || model.renderAabbMin.x > model.renderAabbMax.x) {
					continue;
				}
				const float lo[3]{ model.aabbMin.x, model.aabbMin.y, model.aabbMin.z };
				const float hi[3]{ model.aabbMax.x, model.aabbMax.y, model.aabbMax.z };
				const float renderLo[3]{ model.renderAabbMin.x, model.renderAabbMin.y, model.renderAabbMin.z };
				const float renderHi[3]{ model.renderAabbMax.x, model.renderAabbMax.y, model.renderAabbMax.z };
				float worst = 0.0f;
				for (int axis = 0; axis < 3; ++axis) {
					const auto slack = 16.0f + 0.1f * (renderHi[axis] - renderLo[axis]);
					worst = (std::max)(worst, renderLo[axis] - slack - lo[axis]);
					worst = (std::max)(worst, hi[axis] - renderHi[axis] - slack);
				}
				++boundsChecked;
				if (worst <= 0.0f) {
					++boundsInside;
				} else {
					boundsOutliers.emplace_back(worst, models[i].first);
				}
			}
			spdlog::info("  collision within render bounds: {}/{}", boundsInside, boundsChecked);
			std::ranges::sort(boundsOutliers, std::greater{});
			for (std::size_t i = 0; i < (std::min<std::size_t>)(15, boundsOutliers.size()); ++i) {
				spdlog::info("  outside render bounds by {:.0f}: {}", boundsOutliers[i].first, boundsOutliers[i].second);
			}

			// Per-model counts, for diffing extraction between builds or platforms.
			if (const char* dumpPath = std::getenv("FASTERNGIO_SURVEY_DUMP")) {
				std::vector<std::size_t> order(models.size());
				std::iota(order.begin(), order.end(), std::size_t{ 0 });
				std::ranges::sort(order, [&](std::size_t a, std::size_t b) { return models[a].first < models[b].first; });
				std::ofstream dump(dumpPath);
				for (const auto i : order) {
					dump << models[i].first << '\t' << static_cast<int>(results[i].status) << '\t' << results[i].triangles.size() << '\t' << results[i].hulls.size() << '\t'
					     << results[i].capsules.size() << '\n';
				}
			}

			std::ranges::sort(noCollision, std::greater{});
			for (std::size_t i = 0; i < (std::min<std::size_t>)(25, noCollision.size()); ++i) {
				spdlog::info("  no collision: {} ({} references)", noCollision[i].second, noCollision[i].first);
			}
			return 0;
		}

		// Placement only (no rejection, no caches): every blade of the selected cells, for offline analysis.
		int ExportBlades(const GenerateOptions& a_options, const Grass::PlacementSettings& a_placement, const GameData::StaticWorldSnapshot& a_snapshot,
			std::span<const GameData::LandInfo* const> a_lands)
		{
			const auto begin = std::chrono::steady_clock::now();
			std::vector<Grass::CellCandidates> cells(a_lands.size());
			oneapi::tbb::parallel_for(std::size_t{ 0 }, cells.size(), [&](std::size_t i) {
				cells[i] = Grass::GenerateCellCandidates(a_snapshot, *a_lands[i], a_placement);
			});
			std::ofstream output(a_options.exportBladesPath, std::ios::binary);
			if (!output) {
				throw std::runtime_error("cannot write " + a_options.exportBladesPath.string());
			}
			std::uint64_t blades = 0;
			for (const auto& cell : cells) {
				for (const auto& blade : cell.blades) {
					const float xy[2]{ blade.position[0], blade.position[1] };
					const std::uint32_t grass = cell.groups[blade.groupIndex].grass->formID.value;
					output.write(reinterpret_cast<const char*>(xy), sizeof(xy));
					output.write(reinterpret_cast<const char*>(&grass), sizeof(grass));
					++blades;
				}
			}
			spdlog::info("exported {} blade(s) from {} cell(s) to {} in {:.2f}s", blades, cells.size(), a_options.exportBladesPath.string(), SecondsSince(begin));
			return 0;
		}

		[[nodiscard]] std::uint64_t CountBits(std::span<const std::uint32_t> a_words)
		{
			std::uint64_t count = 0;
			for (const auto word : a_words) {
				count += static_cast<std::uint64_t>(std::popcount(word));
			}
			return count;
		}

		// Rejection throughput without placement or file writing in the measurement: every cell is
		// placed up front, then the same queries go through each backend.
		int BenchmarkRejection(const GenerateOptions& a_options, const LoadedPlugins& a_plugins, const Grass::PlacementSettings& a_placement,
			std::span<const GameData::LandInfo* const> a_lands)
		{
			if (a_options.rejection == RejectChoice::None) {
				throw std::invalid_argument("--benchmark-rejection needs rejection enabled");
			}
			const auto& snapshot = a_plugins.snapshot;
			const auto shapes = MakeQueryShapes(snapshot, a_options.rejectionConfig);
			const auto resolver = MakeResolver(a_options, a_plugins);
			const auto world = BuildWorldIndex(snapshot, a_options.worldFormID, resolver, shapes.maxReach);

			struct Cell
			{
				Grass::CellCandidates candidates;
				std::vector<Rejection::QueryShape> shapes;
				std::vector<std::uint32_t> cpuRejected;
			};
			std::vector<Cell> cells(a_lands.size());
			auto begin = std::chrono::steady_clock::now();
			oneapi::tbb::parallel_for(std::size_t{ 0 }, cells.size(), [&](std::size_t i) {
				auto& cell = cells[i];
				cell.candidates = Grass::GenerateCellCandidates(snapshot, *a_lands[i], a_placement);
				for (const auto& group : cell.candidates.groups) {
					cell.shapes.push_back(shapes.byGrass.at(group.grass->formID));
				}
			});
			// One query per blade.
			std::uint64_t queries = 0;
			for (const auto& cell : cells) {
				queries += cell.candidates.blades.size();
			}
			spdlog::info("benchmark: placed {} blade(s) in {} cell(s) in {:.3f}s", queries, cells.size(), SecondsSince(begin));

			const auto report = [&](const std::string& a_name, double a_seconds, std::uint64_t a_rejected) {
				spdlog::info("benchmark: {:<24} {:8.3f}s  {:7.2f} M queries/s  rejected={}", a_name, a_seconds, static_cast<double>(queries) / a_seconds / 1.0e6, a_rejected);
			};

			begin = std::chrono::steady_clock::now();
			const Rejection::CpuBvh bvh(*world);
			spdlog::info("benchmark: CPU BVH built in {:.3f}s", SecondsSince(begin));
			// Every measurement is repeated; the best run is reported (the first pays for warm-up).
			constexpr int kRepeats = 5;
			std::atomic<std::uint64_t> cpuRejected{ 0 };
			double best = 1.0e30;
			for (int repeat = 0; repeat < kRepeats; ++repeat) {
				cpuRejected = 0;
				begin = std::chrono::steady_clock::now();
				oneapi::tbb::parallel_for(std::size_t{ 0 }, cells.size(), [&](std::size_t i) {
					cells[i].cpuRejected = bvh.RejectCell(cells[i].candidates, cells[i].shapes);
					cpuRejected.fetch_add(CountBits(cells[i].cpuRejected), std::memory_order_relaxed);
				});
				best = (std::min)(best, SecondsSince(begin));
			}
			report(std::format("CPU BVH, {} threads", oneapi::tbb::this_task_arena::max_concurrency()), best, cpuRejected.load());

			oneapi::tbb::task_arena single(1);
			std::uint64_t singleRejected = 0;
			begin = std::chrono::steady_clock::now();
			single.execute([&] {
				for (const auto& cell : cells) {
					singleRejected += CountBits(bvh.RejectCell(cell.candidates, cell.shapes));
				}
			});
			report("CPU BVH, 1 thread", SecondsSince(begin), singleRejected);

#if FASTERNGIO_HAS_GPU
			std::unique_ptr<Gpu::GpuRejector> gpu;
			if (a_options.rejection == RejectChoice::Auto || a_options.rejection == RejectChoice::Gpu) {
				try {
					gpu = CreateGpuRejector(a_options, shapes);
				} catch (const std::exception& e) {
					if (a_options.rejection == RejectChoice::Gpu) {
						throw;
					}
					spdlog::warn("benchmark: no GPU measurement: {}", e.what());
				}
			}
			if (gpu) {
				// Posts the jobs and returns the seconds until the last completes.
				const auto waitAll = [](const std::vector<std::shared_ptr<Gpu::TraceJob>>& a_jobs, const std::function<void()>& a_post) {
					std::atomic<std::size_t> remaining{ a_jobs.size() };
					const auto done = [&remaining] {
						if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
							remaining.notify_all();
						}
					};
					for (const auto& job : a_jobs) {
						if (!job->Subscribe(done)) {
							done();
						}
					}
					const auto start = std::chrono::steady_clock::now();
					a_post();
					Concurrency::WaitUntil(remaining, [](std::size_t a_left) { return a_left == 0; });
					return SecondsSince(start);
				};
				const auto postAll = [&](const std::vector<std::shared_ptr<Gpu::TraceJob>>& a_jobs) {
					return [&gpu, &a_jobs] {
						for (const auto& job : a_jobs) {
							gpu->Post(job);
						}
					};
				};
				// The world build is not part of the measurement: wait for a one-query job behind it.
				begin = std::chrono::steady_clock::now();
				gpu->PostWorld(world);
				const std::vector<std::shared_ptr<Gpu::TraceJob>> warmup{ std::make_shared<Gpu::TraceJob>(std::vector<Gpu::Query>{ Gpu::Query{} }) };
				waitAll(warmup, postAll(warmup));
				spdlog::info("benchmark: GPU world built in {:.3f}s", SecondsSince(begin));

				// One job per cell, as the pipeline posts them.
				std::vector<std::shared_ptr<Gpu::TraceJob>> jobs;
				double perCell = 1.0e30;
				for (int repeat = 0; repeat < kRepeats; ++repeat) {
					jobs.clear();
					for (const auto& cell : cells) {
						jobs.push_back(std::make_shared<Gpu::TraceJob>(Gpu::MakeQueries(cell.candidates.blades, cell.shapes)));
					}
					perCell = (std::min)(perCell, waitAll(jobs, postAll(jobs)));
				}
				std::uint64_t gpuRejected = 0;
				std::uint64_t differ = 0;
				for (std::size_t i = 0; i < cells.size(); ++i) {
					const auto hits = jobs[i]->Hits();
					for (std::size_t b = 0; b < hits.size(); ++b) {
						const bool cpuHit = ((cells[i].cpuRejected[b / 32] >> (b % 32)) & 1u) != 0;
						gpuRejected += hits[b] != 0 ? 1 : 0;
						differ += (hits[b] != 0) != cpuHit ? 1 : 0;
					}
				}
				report(std::format("GPU {}, per-cell jobs", Gpu::GpuApiName(a_options.gpuApi)), perCell, gpuRejected);

				// The same queries in a few large jobs: the GPU's own throughput, without per-job overhead.
				constexpr std::size_t kBulkQueries = 1u << 20;
				std::vector<std::shared_ptr<Gpu::TraceJob>> large;
				double bulk = 1.0e30;
				for (int repeat = 0; repeat < kRepeats; ++repeat) {
					large.clear();
					std::vector<Gpu::Query> pending;
					for (const auto& cell : cells) {
						std::ranges::move(Gpu::MakeQueries(cell.candidates.blades, cell.shapes), std::back_inserter(pending));
						if (pending.size() >= kBulkQueries) {
							large.push_back(std::make_shared<Gpu::TraceJob>(std::move(pending)));
							pending = {};
						}
					}
					if (!pending.empty()) {
						large.push_back(std::make_shared<Gpu::TraceJob>(std::move(pending)));
					}
					bulk = (std::min)(bulk, waitAll(large, postAll(large)));
				}
				std::uint64_t bulkRejected = 0;
				for (const auto& job : large) {
					bulkRejected += static_cast<std::uint64_t>(std::ranges::count_if(job->Hits(), [](std::uint32_t a_hit) { return a_hit != 0; }));
				}
				report(std::format("GPU {}, 1M-query jobs", Gpu::GpuApiName(a_options.gpuApi)), bulk, bulkRejected);
				spdlog::info("benchmark: GPU and CPU BVH disagree on {} of {} queries", differ, queries);
			}
#endif
			return 0;
		}
	}

	int RunDiagnostic(const GenerateOptions& a_options, const LoadedPlugins& a_plugins)
	{
		if (!a_options.dumpCollisionModel.empty()) {
			return DumpCollision(a_options, a_plugins);
		}
		if (a_options.collisionSurvey) {
			return CollisionSurvey(a_options, a_plugins);
		}
		// Export and benchmark place grass as generation would.
		const auto lands = SelectLands(a_plugins.snapshot, a_options.worldFormID, a_options);
		const auto placement = PrepareWorldPlacement(a_plugins.snapshot, a_options.worldFormID, ResolvePlacementSettings(a_options));
		if (!a_options.exportBladesPath.empty()) {
			return ExportBlades(a_options, placement.settings, a_plugins.snapshot, lands);
		}
		return BenchmarkRejection(a_options, a_plugins, placement.settings, lands);
	}
}
