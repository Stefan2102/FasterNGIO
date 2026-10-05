#include "Generate.h"

#include "Diagnostics.h"
#include "WorldSetup.h"

#include "Grass/CellCache.h"
#include "Pipeline/CellPipeline.h"
#include "Pipeline/FileWriterPool.h"
#include "Rejection/CpuBvh.h"

#include <oneapi/tbb/parallel_for.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace FasterNGIO::App
{
	namespace
	{
		using Pipeline::RejectionBackend;

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
		// query shapes, the writer, the archives and the GPU rejector.
		struct RunShared
		{
			const GenerateOptions& options;
			const RunControl& control;
			const LoadedPlugins& plugins;
			const Grass::PlacementSettings& placement;
			const QueryShapes& shapes;
			// When the run started, for the "total" in each world's summary.
			std::chrono::steady_clock::time_point begin;
			// Writes every world's files, so a world's last files overlap the next world's preparation.
			Pipeline::FileWriterPool& writer;
			// Opened for the first world with rejection, then shared (read-only).
			std::optional<Archives::ArchiveResolver> resolver{};
			// Set when the GPU turned out to be unsupported, so later worlds go straight to the CPU BVH.
			bool gpuUnavailable{ false };
#if FASTERNGIO_HAS_GPU
			// Created for the first world that needs it; each later world replaces the previous one.
			std::unique_ptr<Gpu::GpuRejector> gpu{};
#endif
		};

		// One world's rejection: the backend in use and what it traces against.
		struct WorldRejection
		{
			RejectionBackend backend{ RejectionBackend::None };
			std::shared_ptr<const Rejection::WorldIndex> index;
			std::unique_ptr<Rejection::CpuBvh> cpuBvh;
			Gpu::GpuRejector* gpu{ nullptr };
		};

		// The GPU when it is wanted and works (device and pipeline creation, the shader compile on
		// Windows, is the only step that waits on the render thread), else the CPU BVH; the collision
		// index and whichever structure the backend traces. The GPU builds its world while the CPU
		// places grass.
		[[nodiscard]] WorldRejection SetUpRejection(RunShared& a_shared, GameData::FormID a_worldFormID)
		{
			const auto& options = a_shared.options;
			WorldRejection rejection;
			if (options.rejection == RejectChoice::None) {
				spdlog::info("rejection: off");
				return rejection;
			}
			rejection.backend = RejectionBackend::Cpu;
#if FASTERNGIO_HAS_GPU
			if (!a_shared.gpu && !a_shared.gpuUnavailable && (options.rejection == RejectChoice::Auto || options.rejection == RejectChoice::Gpu)) {
				try {
					a_shared.gpu = CreateGpuRejector(options, a_shared.shapes);
				} catch (const std::exception& e) {
					if (options.rejection == RejectChoice::Gpu) {
						throw;
					}
					a_shared.gpuUnavailable = true;
					spdlog::warn("GPU rejection unavailable, using the CPU BVH: {}", e.what());
				}
			}
			if (a_shared.gpu) {
				rejection.gpu = a_shared.gpu.get();
				rejection.backend = RejectionBackend::Gpu;
			}
#endif
			spdlog::info("rejection: {}", rejection.backend == RejectionBackend::Gpu ? "GPU" : "CPU BVH");

			if (!a_shared.resolver) {
				a_shared.resolver.emplace(MakeResolver(options, a_shared.plugins));
			}
			rejection.index = BuildWorldIndex(a_shared.plugins.snapshot, a_worldFormID, *a_shared.resolver, a_shared.shapes.maxReach);
			ThrowIfStopped(a_shared.control);
			if (rejection.backend == RejectionBackend::Cpu) {
				rejection.cpuBvh = std::make_unique<Rejection::CpuBvh>(*rejection.index);
				const auto& stats = rejection.cpuBvh->Stats();
				spdlog::info("cpu bvh: {} model BVH(s) over {} primitive(s) ({} nodes), {} instance(s) ({} nodes) built in {:.3f}s", stats.models, stats.primitives,
					stats.modelNodes, stats.instances, stats.instanceNodes, stats.buildSeconds);
				ThrowIfStopped(a_shared.control);
			}
#if FASTERNGIO_HAS_GPU
			if (rejection.gpu) {
				rejection.gpu->PostWorld(rejection.index);
			}
#endif
			return rejection;
		}

		void LogWorldSummary(const RunShared& a_shared, const WorldRejection& a_rejection, const Pipeline::CellPipelineStats& a_stats,
			std::chrono::steady_clock::time_point a_generationBegin)
		{
#if FASTERNGIO_HAS_GPU
			if (a_rejection.gpu) {
				const auto gpuStats = a_rejection.gpu->Stats();
				spdlog::info("gpu: {} BLAS(es) ({:.1f} MiB, model data {:.1f} MiB), TLAS of {} instance(s) built in {:.2f}s; traced {} quer(ies) in {} frame(s)",
					gpuStats.models, static_cast<double>(gpuStats.blasBytes) / (1 << 20), static_cast<double>(gpuStats.modelBytes) / (1 << 20), gpuStats.instances,
					gpuStats.worldBuildSeconds, gpuStats.queries, gpuStats.framesExecuted);
			}
#else
			(void)a_rejection;
#endif
			if (a_shared.options.validateCpu) {
				spdlog::info("validation: {} blade(s) differ from the brute-force CPU reference", a_stats.validationMismatches);
			}
			spdlog::info("generated {} file(s), skipped {}, failed {}, blades={} rejected={} in {:.2f}s (total {:.2f}s, {} file(s) still queued)", a_stats.cellsWritten,
				a_stats.cellsSkipped, a_stats.cellsFailed, a_stats.blades, a_stats.bladesRejected, SecondsSince(a_generationBegin), SecondsSince(a_shared.begin),
				a_shared.writer.PendingFiles());
		}

		struct WorldResult
		{
			Pipeline::CellPipelineStats stats{};
			// What became of the files handed to the writer; complete once it has drained.
			std::shared_ptr<Pipeline::WriteTally> tally{};
		};

		WorldResult RunWorld(RunShared& a_shared, GameData::FormID a_worldFormID)
		{
			const auto& options = a_shared.options;
			const auto& control = a_shared.control;
			const auto& snapshot = a_shared.plugins.snapshot;
			SetStage(control, RunStage::Preparing);
			auto lands = SelectLands(snapshot, a_worldFormID, options);
			if (control.progress) {
				control.progress->cellsDone.store(0, std::memory_order_relaxed);
				control.progress->cellsTotal.store(static_cast<std::uint32_t>(lands.size()), std::memory_order_relaxed);
			}
			const auto placement = PrepareWorldPlacement(snapshot, a_worldFormID, a_shared.placement);
			ThrowIfStopped(control);

			const auto worldEditorID = Grass::ResolveWorldEditorID(snapshot, a_worldFormID);
			std::filesystem::create_directories(options.outputDirectory);
			spdlog::info("world {} ({:08X}): {} cell(s) -> {}", worldEditorID, a_worldFormID.value, lands.size(), options.outputDirectory.string());

			const auto generationBegin = std::chrono::steady_clock::now();
			const auto rejection = SetUpRejection(a_shared, a_worldFormID);

			auto tally = std::make_shared<Pipeline::WriteTally>();
			Pipeline::CellPipelineDesc pipeline;
			pipeline.snapshot = &snapshot;
			pipeline.lands = std::move(lands);
			pipeline.worldEditorID = worldEditorID;
			pipeline.outputDirectory = options.outputDirectory;
			pipeline.placement = placement.settings;
			pipeline.overwrite = options.overwrite;
			pipeline.shapesByGrass = &a_shared.shapes.byGrass;
			pipeline.backend = rejection.backend;
			pipeline.world = rejection.index.get();
			pipeline.cpuBvh = rejection.cpuBvh.get();
			pipeline.gpu = rejection.gpu;
			pipeline.validateCpu = options.validateCpu;
			pipeline.progress = control.progress ? std::addressof(control.progress->cellsDone) : nullptr;
			pipeline.stop = control.stop;
			pipeline.writer = std::addressof(a_shared.writer);
			pipeline.writeTally = tally;
			SetStage(control, RunStage::Generating);
			const auto stats = Pipeline::RunCellPipeline(pipeline);
			LogWorldSummary(a_shared, rejection, stats, generationBegin);
			return WorldResult{ .stats = stats, .tally = std::move(tally) };
		}

		// The worldspaces a run generates, in order.
		[[nodiscard]] std::vector<GameData::FormID> SelectWorlds(const GenerateOptions& a_options, const GameData::StaticWorldSnapshot& a_snapshot)
		{
			const auto available = ListWorlds(a_snapshot);
			std::vector<GameData::FormID> worlds;
			if (a_options.allWorlds) {
				for (const auto& summary : available) {
					worlds.push_back(summary.formID);
				}
				if (worlds.empty()) {
					throw std::runtime_error("no worldspace has LAND records");
				}
				spdlog::info("{} worldspace(s) with LAND records", worlds.size());
				return worlds;
			}
			// Checked before the first world runs, so a mistyped one does not stop the run halfway.
			for (const auto world : a_options.worlds) {
				if (std::ranges::find(available, world, &WorldSummary::formID) == available.end()) {
					throw std::runtime_error(std::format("no LAND records were loaded for world {:08X}", world.value));
				}
			}
			if (a_options.worlds.size() > 1) {
				spdlog::info("{} worldspace(s) selected", a_options.worlds.size());
			}
			return a_options.worlds;
		}
	}

	LoadedPlugins LoadStaticSnapshot(const GenerateOptions& a_options)
	{
		const auto begin = std::chrono::steady_clock::now();
		auto loadOrder = GameData::PrepareLoadOrder(GameData::ReadPluginsTxt(a_options.dataPath, a_options.pluginsTxtPath));
		spdlog::info("load order: {} plugin(s) from {}", loadOrder.size(), a_options.pluginsTxtPath.string());

		std::vector<GameData::StaticPluginShard> shards(loadOrder.size());
		oneapi::tbb::parallel_for(std::size_t{ 0 }, loadOrder.size(), [&](std::size_t i) {
			shards[i] = GameData::ParseStaticWorldShard(loadOrder[i], loadOrder);
		});
		spdlog::info("parsed plugins in {:.2f}s", SecondsSince(begin));

		const auto snapshotBegin = std::chrono::steady_clock::now();
		auto snapshot = GameData::BuildStaticWorldSnapshot(shards);
		spdlog::info("snapshot: worlds={} cells={} ltex={} gras={} base_objects={} exterior_cells={} ({:.2f}s)", snapshot.worldsByFormID.size(),
			snapshot.cellsByFormID.size(), snapshot.landTexturesByFormID.size(), snapshot.grassesByFormID.size(), snapshot.baseObjectsByFormID.size(),
			snapshot.exteriorPlacementsByCell.size(), SecondsSince(snapshotBegin));
		return LoadedPlugins{ .loadOrder = std::move(loadOrder), .snapshot = std::move(snapshot) };
	}

	std::vector<WorldSummary> ListWorlds(const GameData::StaticWorldSnapshot& a_snapshot)
	{
		std::vector<WorldSummary> worlds;
		for (const auto& [formID, lands] : a_snapshot.landsByWorldspace) {
			std::unordered_set<std::uint64_t> cells;
			for (const auto& land : lands) {
				if (land.cellX && land.cellY) {
					cells.insert(GameData::PackCellCoords(*land.cellX, *land.cellY));
				}
			}
			if (!cells.empty()) {
				worlds.push_back(WorldSummary{ .formID = formID, .editorID = Grass::ResolveWorldEditorID(a_snapshot, formID), .cells = cells.size() });
			}
		}
		std::ranges::sort(worlds, {}, [](const WorldSummary& a_world) { return a_world.formID.value; });
		return worlds;
	}

	RunResult Run(const GenerateOptions& a_options, const RunControl& a_control)
	{
		RunResult result;
		// Files queue in memory up to this many bytes; past it, cells wait to be placed.
		constexpr std::uint64_t kMaxPendingWriteBytes = 512ull << 20;
		// Outside the try: a cancelled run still writes what it finished.
		std::optional<Pipeline::FileWriterPool> writer;
		std::vector<std::shared_ptr<Pipeline::WriteTally>> tallies;
		std::uint64_t blades = 0;
		std::uint64_t rejected = 0;
		const auto begin = std::chrono::steady_clock::now();
		try {
			SetStage(a_control, RunStage::LoadingPlugins);
			std::optional<LoadedPlugins> loaded;
			if (!a_control.preloaded) {
				loaded.emplace(LoadStaticSnapshot(a_options));
			}
			const auto& plugins = a_control.preloaded ? *a_control.preloaded : *loaded;
			ThrowIfStopped(a_control);
			if (a_options.RunsDiagnostic()) {
				result.exitCode = RunDiagnostic(a_options, plugins);
				SetStage(a_control, RunStage::Finished);
				return result;
			}

			const auto worlds = SelectWorlds(a_options, plugins.snapshot);
			const auto placement = ResolvePlacementSettings(a_options);
			const auto shapes = MakeQueryShapes(plugins.snapshot, a_options.rejectionConfig);
			writer.emplace(a_options.writerThreads, kMaxPendingWriteBytes);
			RunShared shared{
				.options = a_options,
				.control = a_control,
				.plugins = plugins,
				.placement = placement,
				.shapes = shapes,
				.begin = begin,
				.writer = *writer,
			};
			if (a_control.progress) {
				a_control.progress->worldCount.store(static_cast<std::uint32_t>(worlds.size()), std::memory_order_relaxed);
			}
			for (std::size_t i = 0; i < worlds.size(); ++i) {
				ThrowIfStopped(a_control);
				if (a_control.progress) {
					a_control.progress->worldIndex.store(static_cast<std::uint32_t>(i), std::memory_order_relaxed);
					a_control.progress->worldFormID.store(worlds[i].value, std::memory_order_relaxed);
				}
				const auto world = RunWorld(shared, worlds[i]);
				++result.worlds;
				tallies.push_back(world.tally);
				result.cellsSkipped += world.stats.cellsSkipped;
				result.cellsFailed += world.stats.cellsFailed;
				blades += world.stats.blades;
				rejected += world.stats.bladesRejected;
				if (world.stats.cellsCancelled != 0) {
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
				spdlog::info("writers: {} file(s) in {:.2f}s of filesystem time over {} thread(s) ({:.0f} us per file); waited {:.2f}s for the last ones", files,
					writer->BusySeconds(), a_options.writerThreads, writer->BusySeconds() * 1.0e6 / static_cast<double>(files), SecondsSince(drainBegin));
			}
			for (const auto& tally : tallies) {
				result.cellsWritten += tally->written.load();
				result.cellsFailed += tally->failed.load();
			}
			if (result.cancelled) {
				spdlog::warn("cancelled after {} worldspace(s): wrote {} file(s)", result.worlds, result.cellsWritten);
			} else {
				spdlog::info("{}wrote {} file(s), skipped {}, failed {}, blades={} rejected={} in {:.2f}s",
					result.worlds > 1 ? std::format("all {} worldspace(s): ", result.worlds) : std::string{}, result.cellsWritten, result.cellsSkipped, result.cellsFailed,
					blades, rejected, SecondsSince(begin));
			}
		}
		SetStage(a_control, RunStage::Finished);
		if (result.cellsFailed != 0 || result.cancelled) {
			result.exitCode = 1;
		}
		return result;
	}
}
