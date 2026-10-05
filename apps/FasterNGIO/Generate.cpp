#include "Generate.h"

#include "Archives/ArchiveResolver.h"
#include "Collision/NifCollisionExtractor.h"
#include "Collision/ObjExport.h"
#include "GameData/GameData.h"
#include "Grass/GameIni.h"
#include "Grass/NgioCacheWriter.h"
#include "Grass/Placement.h"
#include "Pipeline/CacheWriter.h"
#include "Pipeline/CellPipeline.h"
#include "Rejection/CpuBvh.h"
#include "Rejection/CpuReference.h"
#include "Rejection/RejectionConfig.h"
#include "Rejection/WorldIndex.h"
#if FASTERNGIO_HAS_GPU
#include "Gpu/GpuRejector.h"
#endif

#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/task_arena.h>
#include <spdlog/spdlog.h>

#if defined(_WIN32)
#include <Windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace FasterNGIO::App
{
	namespace
	{
		using Pipeline::RejectionBackend;

		[[nodiscard]] std::filesystem::path ExecutableDirectory()
		{
#if defined(_WIN32)
			wchar_t buffer[MAX_PATH]{};
			GetModuleFileNameW(nullptr, buffer, MAX_PATH);
			return std::filesystem::path(buffer).parent_path();
#else
			std::error_code error;
			return std::filesystem::read_symlink("/proc/self/exe", error).parent_path();
#endif
		}

		[[nodiscard]] double SecondsSince(std::chrono::steady_clock::time_point a_begin)
		{
			return std::chrono::duration<double>(std::chrono::steady_clock::now() - a_begin).count();
		}

		[[nodiscard]] bool IsSelectedCell(const CliOptions& a_options, std::int32_t a_x, std::int32_t a_y)
		{
			if (a_options.singleCellX && a_options.singleCellY) {
				return a_x == *a_options.singleCellX && a_y == *a_options.singleCellY;
			}
			if (a_options.centerCellX && a_options.centerCellY && a_options.radius) {
				return std::abs(a_x - *a_options.centerCellX) <= *a_options.radius && std::abs(a_y - *a_options.centerCellY) <= *a_options.radius;
			}
			return true;
		}

		[[nodiscard]] Archives::ArchiveResolver MakeResolver(const CliOptions& a_options, const LoadedWorld& a_world)
		{
			const auto order = Archives::DefaultArchiveOrder(a_world.loadOrder);
			return Archives::ArchiveResolver(a_options.dataPath, order);
		}

		int DumpCollision(const CliOptions& a_options)
		{
			const auto world = LoadStaticSnapshot(a_options);
			const auto resolver = MakeResolver(a_options, world);
			const auto modelPath = GameData::NormalizeModelPath(a_options.dumpCollisionModel);
			const auto bytes = resolver.Read(modelPath);
			if (!bytes) {
				throw std::runtime_error("model not found: " + modelPath);
			}
			const auto model = Collision::ExtractCollision(*bytes, Collision::ExtractionOptions{ .trace = true, .renderBounds = true });
			for (const auto& line : model.stats.trace) {
				spdlog::info("{}", line);
			}
			spdlog::info(
				"{}: status={} triangles={} hulls={} hull_triangles={} capsules={} bodies_kept={} filtered={}",
				modelPath,
				static_cast<int>(model.status),
				model.triangles.size(),
				model.hulls.size(),
				model.hullTriangles.size(),
				model.capsules.size(),
				model.stats.bodiesKept,
				model.stats.bodiesFilteredByLayer);
			spdlog::info(
				"collision aabb ({:.0f}, {:.0f}, {:.0f}) - ({:.0f}, {:.0f}, {:.0f})",
				model.aabbMin.x,
				model.aabbMin.y,
				model.aabbMin.z,
				model.aabbMax.x,
				model.aabbMax.y,
				model.aabbMax.z);
			spdlog::info(
				"render aabb ({:.0f}, {:.0f}, {:.0f}) - ({:.0f}, {:.0f}, {:.0f})",
				model.renderAabbMin.x,
				model.renderAabbMin.y,
				model.renderAabbMin.z,
				model.renderAabbMax.x,
				model.renderAabbMax.y,
				model.renderAabbMax.z);
			for (const auto& [formID, base] : world.snapshot.baseObjectsByFormID) {
				if (base.bounds.present && GameData::NormalizeModelPath(base.modelPath) == modelPath) {
					const auto& b = base.bounds;
					spdlog::info("OBND {:08X}: ({}, {}, {}) - ({}, {}, {})", formID.value, b.min[0], b.min[1], b.min[2], b.max[0], b.max[1], b.max[2]);
				}
			}
			Collision::WriteObj(model, a_options.dumpCollisionPath);
			return 0;
		}

		int CollisionSurvey(const CliOptions& a_options)
		{
			const auto world = LoadStaticSnapshot(a_options);
			const auto& snapshot = world.snapshot;
			const auto resolver = MakeResolver(a_options, world);

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

			std::array<std::uint64_t, 4> modelsByStatus{};
			std::array<std::uint64_t, 4> referencesByStatus{};
			std::array<std::uint64_t, static_cast<std::size_t>(Collision::ShapeType::Count)> shapes{};
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

			constexpr std::array<const char*, 4> statusNames{ "has_collision", "filtered_by_layer", "no_collision", "load_failed" };
			spdlog::info("extracted {} model(s) in {:.2f}s", models.size(), elapsed);
			for (std::size_t s = 0; s < statusNames.size(); ++s) {
				spdlog::info("  {:<18} models={:<6} references={}", statusNames[s], modelsByStatus[s], referencesByStatus[s]);
			}
			spdlog::info("  missing files: {}", missingCount);
			spdlog::info("  primitives: mesh_triangles={} hulls={} hull_triangles={} capsules={}", triangles, hulls, hullTriangles, capsules);
			constexpr std::array<const char*, static_cast<std::size_t>(Collision::ShapeType::Count)> shapeNames{
				"compressed_mesh", "packed_strips", "convex_vertices", "box", "sphere", "capsule", "multi_sphere", "list", "convex_list", "transform", "mopp", "unsupported"
			};
			for (std::size_t s = 0; s < shapes.size(); ++s) {
				if (shapes[s] != 0) {
					spdlog::info("  shape {:<16} {}", shapeNames[s], shapes[s]);
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
		int ExportBlades(const CliOptions& a_options, const Grass::PlacementSettings& a_placement, const GameData::StaticWorldSnapshot& a_snapshot,
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

		using ShapeMap = std::unordered_map<GameData::FormID, Rejection::QueryShape, GameData::FormIDHash>;

		// Rejection throughput without placement or file writing in the measurement: every cell is
		// placed up front, then the same queries go through each backend.
		int BenchmarkRejection([[maybe_unused]] const CliOptions& a_options, const Grass::PlacementSettings& a_placement, const GameData::StaticWorldSnapshot& a_snapshot,
			std::span<const GameData::LandInfo* const> a_lands,
			const ShapeMap& a_shapesByGrass, std::shared_ptr<const Rejection::WorldIndex> a_world, [[maybe_unused]] void* a_gpu)
		{
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
				cell.candidates = Grass::GenerateCellCandidates(a_snapshot, *a_lands[i], a_placement);
				for (const auto& group : cell.candidates.groups) {
					cell.shapes.push_back(a_shapesByGrass.at(group.grass->formID));
				}
			});
			std::uint64_t blades = 0;
			std::uint64_t queries = 0;
			for (const auto& cell : cells) {
				blades += cell.candidates.blades.size();
				for (const auto& blade : cell.candidates.blades) {
					queries += cell.shapes[blade.groupIndex].test ? 1 : 0;
				}
			}
			spdlog::info("benchmark: placed {} blade(s) ({} queries) in {} cell(s) in {:.3f}s", blades, queries, cells.size(), SecondsSince(begin));

			const auto report = [&](const char* a_name, double a_seconds, std::uint64_t a_rejected) {
				spdlog::info("benchmark: {:<24} {:8.3f}s  {:7.2f} M queries/s  rejected={}", a_name, a_seconds, static_cast<double>(queries) / a_seconds / 1.0e6, a_rejected);
			};

			begin = std::chrono::steady_clock::now();
			const Rejection::CpuBvh bvh(*a_world);
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
					std::uint64_t count = 0;
					for (const auto word : cells[i].cpuRejected) {
						count += static_cast<std::uint64_t>(std::popcount(word));
					}
					cpuRejected.fetch_add(count, std::memory_order_relaxed);
				});
				best = (std::min)(best, SecondsSince(begin));
			}
			report(std::format("CPU BVH, {} threads", oneapi::tbb::this_task_arena::max_concurrency()).c_str(), best, cpuRejected.load());

			oneapi::tbb::task_arena single(1);
			std::uint64_t singleRejected = 0;
			begin = std::chrono::steady_clock::now();
			single.execute([&] {
				for (const auto& cell : cells) {
					for (const auto word : bvh.RejectCell(cell.candidates, cell.shapes)) {
						singleRejected += static_cast<std::uint64_t>(std::popcount(word));
					}
				}
			});
			report("CPU BVH, 1 thread", SecondsSince(begin), singleRejected);

#if FASTERNGIO_HAS_GPU
			auto* gpu = static_cast<Gpu::GpuRejector*>(a_gpu);
			if (gpu) {
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
					for (auto left = remaining.load(std::memory_order_acquire); left != 0; left = remaining.load(std::memory_order_acquire)) {
						remaining.wait(left, std::memory_order_acquire);
					}
					return SecondsSince(start);
				};
				const auto makeQueries = [&](const Cell& a_cell, std::vector<Gpu::Query>& a_out, std::vector<std::uint32_t>* a_blades) {
					for (std::uint32_t b = 0; b < a_cell.candidates.blades.size(); ++b) {
						const auto& blade = a_cell.candidates.blades[b];
						const auto& shape = a_cell.shapes[blade.groupIndex];
						if (shape.test) {
							a_out.push_back({ blade.position[0], blade.position[1], blade.position[2] - shape.depth, (std::max)(shape.radius, 1.0e-3f) });
							if (a_blades) {
								a_blades->push_back(b);
							}
						}
					}
				};

				// The world build is not part of the measurement: wait for a one-query job behind it.
				begin = std::chrono::steady_clock::now();
				gpu->PostWorld(a_world);
				const std::vector<std::shared_ptr<Gpu::TraceJob>> warmup{ std::make_shared<Gpu::TraceJob>(std::vector<Gpu::Query>{ Gpu::Query{} }) };
				waitAll(warmup, [&] { gpu->Post(warmup[0]); });
				spdlog::info("benchmark: GPU world built in {:.3f}s", SecondsSince(begin));

				// One job per cell, as the pipeline posts them.
				std::vector<std::shared_ptr<Gpu::TraceJob>> jobs;
				std::vector<std::vector<std::uint32_t>> jobBlades(cells.size());
				double perCell = 1.0e30;
				for (int repeat = 0; repeat < kRepeats; ++repeat) {
					jobs.clear();
					for (std::size_t i = 0; i < cells.size(); ++i) {
						std::vector<Gpu::Query> cellQueries;
						jobBlades[i].clear();
						makeQueries(cells[i], cellQueries, &jobBlades[i]);
						jobs.push_back(std::make_shared<Gpu::TraceJob>(std::move(cellQueries)));
					}
					perCell = (std::min)(perCell, waitAll(jobs, [&] {
						for (const auto& job : jobs) {
							gpu->Post(job);
						}
					}));
				}
				std::uint64_t gpuRejected = 0;
				std::uint64_t differ = 0;
				for (std::size_t i = 0; i < cells.size(); ++i) {
					const auto hits = jobs[i]->Hits();
					for (std::size_t q = 0; q < hits.size(); ++q) {
						const auto blade = jobBlades[i][q];
						const bool cpuHit = ((cells[i].cpuRejected[blade / 32] >> (blade % 32)) & 1u) != 0;
						gpuRejected += hits[q] != 0 ? 1 : 0;
						differ += (hits[q] != 0) != cpuHit ? 1 : 0;
					}
				}
				report(std::format("GPU {}, per-cell jobs", Gpu::GpuApiName(a_options.gpuApi)).c_str(), perCell, gpuRejected);

				// The same queries in a few large jobs: the GPU's own throughput, without per-job overhead.
				std::vector<std::shared_ptr<Gpu::TraceJob>> large;
				double bulk = 1.0e30;
				for (int repeat = 0; repeat < kRepeats; ++repeat) {
					large.clear();
					std::vector<Gpu::Query> pending;
					for (const auto& cell : cells) {
						makeQueries(cell, pending, nullptr);
						if (pending.size() >= (1u << 20)) {
							large.push_back(std::make_shared<Gpu::TraceJob>(std::move(pending)));
							pending = {};
						}
					}
					if (!pending.empty()) {
						large.push_back(std::make_shared<Gpu::TraceJob>(std::move(pending)));
					}
					bulk = (std::min)(bulk, waitAll(large, [&] {
						for (const auto& job : large) {
							gpu->Post(job);
						}
					}));
				}
				std::uint64_t bulkRejected = 0;
				for (const auto& job : large) {
					for (const auto hit : job->Hits()) {
						bulkRejected += hit != 0 ? 1 : 0;
					}
				}
				report(std::format("GPU {}, 1M-query jobs", Gpu::GpuApiName(a_options.gpuApi)).c_str(), bulk, bulkRejected);
				spdlog::info("benchmark: GPU and CPU BVH disagree on {} of {} queries", differ, queries);
			}
#endif
			return 0;
		}

		// Engine defaults, then the game's INIs (as the game would read them), then command-line values.
		[[nodiscard]] Grass::PlacementSettings ResolvePlacementSettings(const CliOptions& a_options)
		{
			auto settings = a_options.placement;
			if (a_options.readGameIni) {
				if (const auto directory = Grass::LocateGameIniDirectory(a_options.pluginsTxtPath, a_options.gameIniDirectory)) {
					const auto ini = Grass::ReadGrassIniSettings(directory->path);
					Grass::ApplyGrassIniSettings(ini, settings);
					if (ini.filesRead.empty()) {
						spdlog::info("game INI: no Skyrim.ini in {} ({}); using engine defaults", directory->path.string(), directory->origin);
					} else {
						spdlog::info("game INI: {} ({})", directory->path.string(), directory->origin);
					}
					const auto describe = [](const auto& a_value) { return a_value ? std::format("{} ({})", a_value->value, a_value->source.filename().string()) : std::string("engine default"); };
					spdlog::info("game INI [Grass]: iMinGrassSize={} iMaxGrassTypesPerTexure={} fTexturePctThreshold={}", describe(ini.minGrassSize),
						describe(ini.maxGrassTypesPerTexture), describe(ini.texturePctThreshold));
				} else if (a_options.gameIniDirectory) {
					throw std::invalid_argument("--game-ini-dir is not a directory: " + a_options.gameIniDirectory->string());
				} else {
					spdlog::info("game INI: none found; using engine defaults");
				}
			}
			if (a_options.cliMaxGrassTypes) {
				settings.maxGrassTypesPerTexture = *a_options.cliMaxGrassTypes;
			}
			if (a_options.cliMinGrassSize) {
				settings.minGrassSize = *a_options.cliMinGrassSize;
			}
			if (a_options.cliAlphaThreshold) {
				settings.alphaThreshold = *a_options.cliAlphaThreshold;
			}
			spdlog::info("placement: {}, iMinGrassSize={} iMaxGrassTypesPerTexure={} fTexturePctThreshold={}",
				settings.mode == Grass::PlacementMode::Smooth ? "smooth" : "vanilla", settings.minGrassSize, settings.maxGrassTypesPerTexture, settings.alphaThreshold);
			return settings;
		}

		struct RunCancelled
		{
		};

		void ThrowIfStopped(const RunControl& a_control)
		{
			if (a_control.stop.stop_requested()) {
				throw RunCancelled{};
			}
		}

		void SetStage(const RunControl& a_control, RunStage a_stage)
		{
			if (a_control.progress) {
				a_control.progress->stage.store(a_stage, std::memory_order_relaxed);
			}
		}

		// What every world shares: the loaded plugins, the resolved placement settings, the per-grass
		// query shapes, the archives and the GPU rejector.
		struct RunShared
		{
			const CliOptions& options;
			const RunControl& control;
			const LoadedWorld& world;
			const Grass::PlacementSettings& placement;
			const ShapeMap& shapesByGrass;
			float maxReach{ 0.0f };
			// When the run started, for the "total" in each world's summary.
			std::chrono::steady_clock::time_point begin;
			// Set when the GPU turned out to be unsupported, so later worlds go straight to the CPU BVH.
			bool gpuUnavailable{ false };
			// Writes every world's files, so a world's last files overlap the next world's preparation.
			Pipeline::CacheWriter* writer{ nullptr };
			// Opened for the first world with rejection, then shared (read-only).
			std::optional<Archives::ArchiveResolver> resolver{};
#if FASTERNGIO_HAS_GPU
			std::optional<Gpu::GpuRejector> gpu{};
#endif
		};

		struct WorldResult
		{
			Pipeline::CellPipelineStats stats{};
			// What became of the files handed to the writer; complete once it has drained.
			std::shared_ptr<Pipeline::WriteTally> tally{};
			// A diagnostic ran instead of generation; its exit code.
			std::optional<int> diagnostic{};
		};

		WorldResult RunWorld(RunShared& a_shared, GameData::FormID a_worldFormID)
		{
			const auto& options = a_shared.options;
			const auto& control = a_shared.control;
			const auto& snapshot = a_shared.world.snapshot;
			SetStage(control, RunStage::Preparing);
			const auto landsIt = snapshot.landsByWorldspace.find(a_worldFormID);
			if (landsIt == snapshot.landsByWorldspace.end() || landsIt->second.empty()) {
				throw std::runtime_error(std::format("no LAND records were loaded for world {:08X}", a_worldFormID.value));
			}

			const auto worldEditorID = Grass::ResolveWorldEditorID(snapshot, a_worldFormID);
			// The first LAND seen for a cell wins, as in the snapshot's own ordering.
			std::vector<const GameData::LandInfo*> lands;
			std::unordered_set<std::uint64_t> seenCells;
			for (const auto& land : landsIt->second) {
				if (!land.cellX || !land.cellY || !IsSelectedCell(options, *land.cellX, *land.cellY)) {
					continue;
				}
				const auto key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(*land.cellX)) << 32) | static_cast<std::uint32_t>(*land.cellY);
				if (seenCells.insert(key).second) {
					lands.push_back(std::addressof(land));
				}
			}
			std::ranges::sort(lands, [](const GameData::LandInfo* lhs, const GameData::LandInfo* rhs) {
				return *lhs->cellY == *rhs->cellY ? *lhs->cellX < *rhs->cellX : *lhs->cellY < *rhs->cellY;
			});
			if (control.progress) {
				control.progress->cellsDone.store(0, std::memory_order_relaxed);
				control.progress->cellsTotal.store(static_cast<std::uint32_t>(lands.size()), std::memory_order_relaxed);
			}

			auto placement = a_shared.placement;
			// Smooth placement reads every cell's weights and its neighbours': built once per world,
			// then shared read-only by the workers.
			std::optional<Grass::SmoothWeightField> smoothField;
			if (placement.mode == Grass::PlacementMode::Smooth) {
				const auto fieldBegin = std::chrono::steady_clock::now();
				smoothField.emplace(snapshot, landsIt->second, placement);
				placement.smooth.field = std::addressof(*smoothField);
				spdlog::info("smooth placement: weight grids for {} grass type(s) over {} cell(s) built in {:.3f}s", smoothField->GridCount(),
					smoothField->CellCount(), SecondsSince(fieldBegin));
				if (placement.smooth.matchVanillaDensity) {
					double vanilla = 0.0;
					double smooth = 0.0;
					for (const auto& [grass, expected] : smoothField->Expected()) {
						vanilla += expected.vanilla;
						smooth += expected.smooth;
						spdlog::debug("density match {:08X}: vanilla {:.0f} smooth {:.0f} scale {:.3f}", grass.value, expected.vanilla, expected.smooth,
							smoothField->DensityScale(grass));
					}
					spdlog::info("smooth placement: per-type density scaled to vanilla's worldspace totals ({:.0f} expected blades vs {:.0f} unscaled)", vanilla, smooth);
				}
			}
			ThrowIfStopped(control);

			if (!options.exportBladesPath.empty()) {
				return WorldResult{ .diagnostic = ExportBlades(options, placement, snapshot, lands) };
			}
			if (!options.benchmarkRejection) {
				std::filesystem::create_directories(options.outputDirectory);
			}
			spdlog::info("world {} ({:08X}): {} cell(s) -> {}", worldEditorID, a_worldFormID.value, lands.size(), options.outputDirectory.string());

			const auto generationBegin = std::chrono::steady_clock::now();
			auto backend = options.rejection == RejectChoice::None ? RejectionBackend::None : RejectionBackend::Cpu;
#if FASTERNGIO_HAS_GPU
			// Device and pipeline creation (shader compile on Windows) is the only step that waits on the render
			// thread; the world is then posted and builds while the CPU places grass. Created for the first world
			// that needs it; each later world replaces the previous one.
			if (!a_shared.gpu && !a_shared.gpuUnavailable && (options.rejection == RejectChoice::Auto || options.rejection == RejectChoice::Gpu)) {
				float maxRadius = 0.0f;
				for (const auto& [formID, shape] : a_shared.shapesByGrass) {
					maxRadius = (std::max)(maxRadius, shape.radius);
				}
				const auto exeDirectory = ExecutableDirectory();
				try {
					a_shared.gpu.emplace(Gpu::GpuRejectorDesc{
						.api = options.gpuApi,
						.shaderDirectory = exeDirectory / "shaders",
						.shaderCacheDirectory = exeDirectory / "shadercache",
						.mode = options.rejectionConfig.mode,
						.segmentLength = options.rejectionConfig.rayDepth + options.rejectionConfig.rayHeight,
						.maxQueryRadius = maxRadius,
						.debugLayer = options.gpuDebugLayer,
					});
				} catch (const std::exception& e) {
					if (options.rejection == RejectChoice::Gpu) {
						throw;
					}
					a_shared.gpuUnavailable = true;
					spdlog::warn("GPU rejection unavailable, using the CPU BVH: {}", e.what());
				}
			}
			auto* gpu = a_shared.gpu ? std::addressof(*a_shared.gpu) : nullptr;
			if (gpu) {
				backend = RejectionBackend::Gpu;
			}
#endif
			spdlog::info("rejection: {}", backend == RejectionBackend::Gpu ? "GPU" : backend == RejectionBackend::Cpu ? "CPU BVH" : "off");

			std::shared_ptr<const Rejection::WorldIndex> worldIndex;
			std::optional<Rejection::CpuBvh> cpuBvh;
			if (backend != RejectionBackend::None) {
				if (!a_shared.resolver) {
					a_shared.resolver.emplace(MakeResolver(options, a_shared.world));
				}
				worldIndex = std::make_shared<const Rejection::WorldIndex>(snapshot, a_worldFormID, *a_shared.resolver, options.rejectionConfig, a_shared.maxReach);
				const auto& stats = worldIndex->Stats();
				spdlog::info(
					"collision: {} model(s), {} with rejecting collision, {} missing ({:.2f}s); {} of {} reference(s) instanced, {} ignored",
					stats.models,
					stats.modelsWithCollision,
					stats.modelsMissing,
					stats.extractSeconds,
					stats.referencesWithCollision,
					stats.references,
					stats.referencesIgnored);
				ThrowIfStopped(control);
			}
			if (options.benchmarkRejection) {
				if (!worldIndex) {
					throw std::invalid_argument("--benchmark-rejection needs rejection enabled");
				}
#if FASTERNGIO_HAS_GPU
				return WorldResult{ .diagnostic = BenchmarkRejection(options, placement, snapshot, lands, a_shared.shapesByGrass, worldIndex, gpu) };
#else
				return WorldResult{ .diagnostic = BenchmarkRejection(options, placement, snapshot, lands, a_shared.shapesByGrass, worldIndex, nullptr) };
#endif
			}
			if (backend == RejectionBackend::Cpu) {
				cpuBvh.emplace(*worldIndex);
				const auto& stats = cpuBvh->Stats();
				spdlog::info("cpu bvh: {} model BVH(s) over {} primitive(s) ({} nodes), {} instance(s) ({} nodes) built in {:.3f}s",
					stats.models,
					stats.primitives,
					stats.modelNodes,
					stats.instances,
					stats.instanceNodes,
					stats.buildSeconds);
				ThrowIfStopped(control);
			}
#if FASTERNGIO_HAS_GPU
			if (gpu) {
				gpu->PostWorld(worldIndex);
			}
#endif

			Pipeline::CellPipelineDesc pipeline;
			pipeline.snapshot = &snapshot;
			pipeline.lands = std::move(lands);
			pipeline.worldEditorID = worldEditorID;
			pipeline.outputDirectory = options.outputDirectory;
			pipeline.placement = placement;
			pipeline.overwrite = options.overwrite;
			pipeline.shapesByGrass = &a_shared.shapesByGrass;
			pipeline.backend = backend;
			pipeline.world = worldIndex.get();
			pipeline.cpuBvh = cpuBvh ? std::addressof(*cpuBvh) : nullptr;
#if FASTERNGIO_HAS_GPU
			pipeline.gpu = gpu;
#endif
			pipeline.validateCpu = options.validateCpu;
			pipeline.progress = control.progress ? std::addressof(control.progress->cellsDone) : nullptr;
			pipeline.stop = control.stop;
			auto tally = std::make_shared<Pipeline::WriteTally>();
			pipeline.writer = a_shared.writer;
			pipeline.writeTally = tally;
			SetStage(control, RunStage::Generating);
			const auto stats = Pipeline::RunCellPipeline(pipeline);

#if FASTERNGIO_HAS_GPU
			if (gpu) {
				const auto gpuStats = gpu->Stats();
				spdlog::info(
					"gpu: {} BLAS(es) ({:.1f} MiB, model data {:.1f} MiB), TLAS of {} instance(s) built in {:.2f}s; traced {} quer(ies) in {} frame(s)",
					gpuStats.models,
					static_cast<double>(gpuStats.blasBytes) / (1 << 20),
					static_cast<double>(gpuStats.modelBytes) / (1 << 20),
					gpuStats.instances,
					gpuStats.worldBuildSeconds,
					gpuStats.queries,
					gpuStats.framesExecuted);
			}
#endif
			if (options.validateCpu) {
				spdlog::info("validation: {} blade(s) differ from the brute-force CPU reference", stats.validationMismatches);
			}
			spdlog::info(
				"generated {} file(s), skipped {}, failed {}, blades={} rejected={} in {:.2f}s (total {:.2f}s, {} file(s) still queued)",
				stats.cellsWritten,
				stats.cellsSkipped,
				stats.cellsFailed,
				stats.blades,
				stats.bladesRejected,
				SecondsSince(generationBegin),
				SecondsSince(a_shared.begin),
				a_shared.writer ? a_shared.writer->PendingFiles() : 0);
			return WorldResult{ .stats = stats, .tally = std::move(tally) };
		}
	}

	LoadedWorld LoadStaticSnapshot(const CliOptions& a_options)
	{
		const auto begin = std::chrono::steady_clock::now();
		GameData::PluginsTxtLoadOrderSource source(a_options.dataPath, a_options.pluginsTxtPath);
		auto loadOrder = GameData::PrepareLoadOrder(source.Load());
		spdlog::info("load order: {} plugin(s) from {}", loadOrder.size(), a_options.pluginsTxtPath.string());

		std::vector<GameData::StaticPluginShard> shards(loadOrder.size());
		oneapi::tbb::parallel_for(std::size_t{ 0 }, loadOrder.size(), [&](std::size_t i) {
			GameData::PluginParser parser;
			shards[i] = parser.ParseStaticWorldShard(loadOrder[i], i, loadOrder);
		});
		spdlog::info("parsed plugins in {:.2f}s", SecondsSince(begin));

		const auto snapshotBegin = std::chrono::steady_clock::now();
		auto snapshot = GameData::BuildStaticWorldSnapshot(shards);
		spdlog::info(
			"snapshot: worlds={} cells={} ltex={} gras={} base_objects={} exterior_cells={} ({:.2f}s)",
			snapshot.worldsByFormID.size(),
			snapshot.cellsByFormID.size(),
			snapshot.landTexturesByFormID.size(),
			snapshot.grassesByFormID.size(),
			snapshot.baseObjectsByFormID.size(),
			snapshot.exteriorCellKeys.size(),
			SecondsSince(snapshotBegin));
		return LoadedWorld{ .loadOrder = std::move(loadOrder), .snapshot = std::move(snapshot) };
	}

	std::vector<WorldSummary> ListWorlds(const GameData::StaticWorldSnapshot& a_snapshot)
	{
		std::vector<WorldSummary> worlds;
		for (const auto& [formID, lands] : a_snapshot.landsByWorldspace) {
			std::unordered_set<std::uint64_t> cells;
			for (const auto& land : lands) {
				if (land.cellX && land.cellY) {
					cells.insert((static_cast<std::uint64_t>(static_cast<std::uint32_t>(*land.cellX)) << 32) | static_cast<std::uint32_t>(*land.cellY));
				}
			}
			if (!cells.empty()) {
				worlds.push_back(WorldSummary{ .formID = formID, .editorID = Grass::ResolveWorldEditorID(a_snapshot, formID), .cells = cells.size() });
			}
		}
		std::ranges::sort(worlds, {}, [](const WorldSummary& a_world) { return a_world.formID.value; });
		return worlds;
	}

	RunResult Run(const CliOptions& a_options, const RunControl& a_control)
	{
		RunResult result;
		// Outside the try: a cancelled run still writes what it finished.
		std::optional<Pipeline::CacheWriter> writer;
		std::vector<std::shared_ptr<Pipeline::WriteTally>> tallies;
		std::uint64_t blades = 0;
		std::uint64_t rejected = 0;
		const auto begin = std::chrono::steady_clock::now();
		try {
			if (!a_options.dumpCollisionModel.empty()) {
				result.exitCode = DumpCollision(a_options);
				return result;
			}
			if (a_options.collisionSurvey) {
				result.exitCode = CollisionSurvey(a_options);
				return result;
			}
			SetStage(a_control, RunStage::LoadingPlugins);
			std::optional<LoadedWorld> loaded;
			if (!a_control.preloaded) {
				loaded.emplace(LoadStaticSnapshot(a_options));
			}
			const auto& world = a_control.preloaded ? *a_control.preloaded : *loaded;
			ThrowIfStopped(a_control);

			std::vector<GameData::FormID> worlds;
			if (a_options.allWorlds) {
				for (const auto& summary : ListWorlds(world.snapshot)) {
					worlds.push_back(summary.formID);
				}
				if (worlds.empty()) {
					throw std::runtime_error("no worldspace has LAND records");
				}
				spdlog::info("{} worldspace(s) with LAND records", worlds.size());
			} else {
				worlds.push_back(a_options.worldFormID);
			}

			const auto placement = ResolvePlacementSettings(a_options);
			ShapeMap shapesByGrass;
			float maxReach = 0.0f;
			for (const auto& [formID, grass] : world.snapshot.grassesByFormID) {
				const auto shape = Rejection::MakeQueryShape(a_options.rejectionConfig, grass);
				shapesByGrass.emplace(formID, shape);
				maxReach = (std::max)(maxReach, shape.Reach());
			}

			// Files queue in memory up to this many bytes; past it, cells wait to be placed.
			constexpr std::uint64_t kMaxPendingWriteBytes = 512ull << 20;
			writer.emplace(a_options.writerThreads, kMaxPendingWriteBytes);
			RunShared shared{ .options = a_options,
				.control = a_control,
				.world = world,
				.placement = placement,
				.shapesByGrass = shapesByGrass,
				.maxReach = maxReach,
				.begin = begin,
				.writer = std::addressof(*writer) };
			if (a_control.progress) {
				a_control.progress->worldCount.store(static_cast<std::uint32_t>(worlds.size()), std::memory_order_relaxed);
			}
			for (std::size_t i = 0; i < worlds.size(); ++i) {
				ThrowIfStopped(a_control);
				if (a_control.progress) {
					a_control.progress->worldIndex.store(static_cast<std::uint32_t>(i), std::memory_order_relaxed);
					a_control.progress->worldFormID.store(worlds[i].value, std::memory_order_relaxed);
				}
				const auto worldResult = RunWorld(shared, worlds[i]);
				if (worldResult.diagnostic) {
					result.exitCode = *worldResult.diagnostic;
					return result;
				}
				const auto& stats = worldResult.stats;
				++result.worlds;
				tallies.push_back(worldResult.tally);
				result.cellsSkipped += stats.cellsSkipped;
				result.cellsFailed += stats.cellsFailed;
				blades += stats.blades;
				rejected += stats.bladesRejected;
				if (stats.cellsCancelled != 0) {
					throw RunCancelled{};
				}
			}
		} catch (const RunCancelled&) {
			result.cancelled = true;
		}

		if (writer) {
			SetStage(a_control, RunStage::Writing);
			const auto drainBegin = std::chrono::steady_clock::now();
			writer->Drain();
			if (const auto files = writer->FilesDone(); files != 0) {
				spdlog::info("writers: {} file(s) in {:.2f}s of filesystem time over {} thread(s) ({:.0f} us per file); waited {:.2f}s for the last ones",
					files, writer->BusySeconds(), a_options.writerThreads, writer->BusySeconds() * 1.0e6 / static_cast<double>(files), SecondsSince(drainBegin));
			}
			for (const auto& tally : tallies) {
				result.cellsWritten += tally->written.load();
				result.cellsFailed += tally->failed.load();
			}
			if (result.cancelled) {
				spdlog::warn("cancelled after {} worldspace(s): wrote {} file(s)", result.worlds, result.cellsWritten);
			} else {
				spdlog::info("{}wrote {} file(s), skipped {}, failed {}, blades={} rejected={} in {:.2f}s",
					result.worlds > 1 ? std::format("all {} worldspace(s): ", result.worlds) : std::string{}, result.cellsWritten, result.cellsSkipped,
					result.cellsFailed, blades, rejected, SecondsSince(begin));
			}
		}
		SetStage(a_control, RunStage::Finished);
		result.exitCode = result.cellsFailed == 0 && !result.cancelled ? result.exitCode : 1;
		return result;
	}
}
