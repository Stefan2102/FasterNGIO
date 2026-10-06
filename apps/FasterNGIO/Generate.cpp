#include "Generate.h"

#include "Diagnostics.h"
#include "NgioConfig.h"
#include "SeasonsConfig.h"
#include "WorldSetup.h"

#include "Grass/CellCache.h"
#include "Grass/GrassModels.h"
#include "Grass/LandTexture.h"
#include "Pipeline/CellPipeline.h"
#include "Pipeline/FileWriterPool.h"
#include "Platform/Text.h"
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
#include <string_view>
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
			// Seasons of Skyrim's seasons that get their own caches (none without Seasons).
			const ResolvedSeasons& seasons;
			// The caches in the output folder when the run started (see ListExistingCaches).
			const std::unordered_set<std::string>* existingFiles{ nullptr };
			// Each grass type's blades per cache block, from its model.
			const Grass::GrassModelLayout& grassModels;
			// Opened before the first world (grass models, then collision), shared read-only.
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
		[[nodiscard]] WorldRejection SetUpRejection(RunShared& a_shared, GameData::FormID a_worldFormID, const Rejection::RejectionFeatures& a_features)
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

			rejection.index = BuildWorldIndex(a_shared.plugins.snapshot, a_worldFormID, *a_shared.resolver, a_features, a_shared.shapes.maxReach);
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
			spdlog::info("generated {} file(s), skipped {}, empty {}, failed {}, blades={} rejected={}{}{} in {:.2f}s (total {:.2f}s, {} file(s) still queued)",
				a_stats.cellsWritten, a_stats.cellsSkipped, a_stats.cellsEmpty, a_stats.cellsFailed, a_stats.blades, a_stats.bladesRejected,
				a_stats.bladesMoved ? std::format(" moved onto cliffs={}", a_stats.bladesMoved) : std::string{},
				a_stats.bladesCapped ? std::format(" over the quadrant cap={}", a_stats.bladesCapped) : std::string{}, SecondsSince(a_generationBegin),
				SecondsSince(a_shared.begin), a_shared.writer.PendingFiles());
		}

		struct WorldResult
		{
			Pipeline::CellPipelineStats stats{};
			// What became of the files handed to the writer; complete once it has drained.
			std::shared_ptr<Pipeline::WriteTally> tally{};
		};

		// The .cgid names (lower case) already in a_directory. An empty cell removes its stale caches
		// when overwriting; one listing spares a remove call per empty cell and name, which the
		// filesystem (and MO2's VFS) charges even for files that are not there.
		std::unordered_set<std::string> ListExistingCaches(const std::filesystem::path& a_directory)
		{
			const auto listBegin = std::chrono::steady_clock::now();
			std::unordered_set<std::string> names;
			std::error_code error;
			for (std::filesystem::directory_iterator it(a_directory, error), end; !error && it != end; it.increment(error)) {
				auto name = Platform::LowerAscii(it->path().filename().string());
				if (name.ends_with(".cgid")) {
					names.insert(std::move(name));
				}
			}
			spdlog::info("output folder: {} existing cache file(s) ({:.2f}s)", names.size(), SecondsSince(listBegin));
			return names;
		}

		// Every GRAS model's blades per cache block (the engine's instances per group) and the grass types
		// whose model is missing, logged.
		Grass::GrassModelLayout LoadGrassModels(const GameData::StaticWorldSnapshot& a_snapshot, const Archives::ArchiveResolver& a_resolver)
		{
			const auto begin = std::chrono::steady_clock::now();
			auto layout = Grass::MeasureGrassModels(a_snapshot, a_resolver);
			spdlog::info("grass models: {} model(s) for {} grass type(s) in {:.2f}s", layout.models, layout.bladesPerBlock.size(), SecondsSince(begin));
			const auto warn = [](const std::vector<std::string>& a_paths, std::string_view a_problem, std::string_view a_consequence) {
				if (a_paths.empty()) {
					return;
				}
				spdlog::warn("{} grass model(s) {}; {}: {}{}", a_paths.size(), a_problem, a_consequence, a_paths.front(),
					a_paths.size() > 1 ? std::format(" and {} more", a_paths.size() - 1) : std::string{});
				for (const auto& path : a_paths) {
					spdlog::debug("grass model {}: {}", a_problem, path);
				}
			};
			warn(layout.missing, "not found in the loose files or archives",
				std::format("the game cannot load them, so their {} grass type(s) are not placed", layout.missingGrass.size()));
			warn(layout.unsupported, "without a BSTriShape as the root's first child", std::format("their cache blocks hold up to {} blades", Grass::kMaxBladesPerBlock));
			return layout;
		}

		void Accumulate(Pipeline::CellPipelineStats& a_total, const Pipeline::CellPipelineStats& a_pass)
		{
			a_total.cellsWritten += a_pass.cellsWritten;
			a_total.cellsSkipped += a_pass.cellsSkipped;
			a_total.cellsEmpty += a_pass.cellsEmpty;
			a_total.cellsFailed += a_pass.cellsFailed;
			a_total.cellsCancelled += a_pass.cellsCancelled;
			a_total.blades += a_pass.blades;
			a_total.bladesRejected += a_pass.bladesRejected;
			a_total.validationMismatches += a_pass.validationMismatches;
			a_total.bladesMoved += a_pass.bladesMoved;
			a_total.bladesCapped += a_pass.bladesCapped;
		}

		[[nodiscard]] std::string DescribePass(const SeasonPass& a_pass)
		{
			std::string names;
			for (const auto& suffix : a_pass.suffixes) {
				names += std::format("{}{}", names.empty() ? "" : ", ", suffix.empty() ? "plain" : suffix);
			}
			if (a_pass.landTextureGrass.empty() && a_pass.baseObjects.empty()) {
				return names;
			}
			return std::format("{} ({} land texture grass swap(s), {} object swap(s))", names, a_pass.landTextureGrass.size(), a_pass.baseObjects.size());
		}

		// One world: a pass per distinct set of season swaps (just the plain one without Seasons of
		// Skyrim), each writing its cells under every name it covers. Passes that swap objects trace
		// against their own collision index; the others share the plain one.
		WorldResult RunWorld(RunShared& a_shared, GameData::FormID a_worldFormID)
		{
			const auto& options = a_shared.options;
			const auto& control = a_shared.control;
			const auto& snapshot = a_shared.plugins.snapshot;
			SetStage(control, RunStage::Preparing);
			const auto lands = SelectLands(snapshot, a_worldFormID, options);
			const auto worldEditorID = Grass::ResolveWorldEditorID(snapshot, a_worldFormID);
			auto passes = PlanSeasonPasses(a_shared.seasons, snapshot, worldEditorID, options.rejection != RejectChoice::None);
			// The passes that share the plain collision run first, so it is posted to the GPU once.
			std::stable_partition(passes.begin() + 1, passes.end(), [](const SeasonPass& a_pass) { return a_pass.baseObjects.empty(); });
			if (control.progress) {
				control.progress->cellsDone.store(0, std::memory_order_relaxed);
				control.progress->cellsTotal.store(static_cast<std::uint32_t>(lands.size() * passes.size()), std::memory_order_relaxed);
			}
			std::filesystem::create_directories(options.outputDirectory);
			spdlog::info("world {} ({:08X}): {} cell(s) -> {}", worldEditorID, a_worldFormID.value, lands.size(), options.outputDirectory.string());
			if (passes.size() > 1 || passes.front().suffixes.size() > 1) {
				for (std::size_t i = 0; i < passes.size(); ++i) {
					spdlog::info("seasons: pass {} of {}: {}", i + 1, passes.size(), DescribePass(passes[i]));
				}
			}

			WorldResult result;
			result.tally = std::make_shared<Pipeline::WriteTally>();
			std::optional<WorldRejection> plainRejection;
			const Rejection::WorldIndex* posted = nullptr;
			for (const auto& pass : passes) {
				ThrowIfStopped(control);
				SetStage(control, RunStage::Preparing);
				auto settings = a_shared.placement;
				settings.landTextureGrass = pass.landTextureGrass.empty() ? nullptr : std::addressof(pass.landTextureGrass);
				const auto placement = PrepareWorldPlacement(snapshot, a_worldFormID, settings);
				ThrowIfStopped(control);

				const auto generationBegin = std::chrono::steady_clock::now();
				std::optional<WorldRejection> seasonalRejection;
				const WorldRejection* rejection = nullptr;
				if (pass.baseObjects.empty()) {
					if (!plainRejection) {
						plainRejection.emplace(SetUpRejection(a_shared, a_worldFormID, options.rejectionFeatures));
						posted = plainRejection->index.get();
					}
#if FASTERNGIO_HAS_GPU
					if (plainRejection->gpu && posted != plainRejection->index.get()) {
						plainRejection->gpu->PostWorld(plainRejection->index);
						posted = plainRejection->index.get();
					}
#endif
					rejection = std::addressof(*plainRejection);
				} else {
					auto features = options.rejectionFeatures;
					features.baseSwaps = pass.baseObjects;
					seasonalRejection.emplace(SetUpRejection(a_shared, a_worldFormID, features));
					posted = seasonalRejection->index.get();
					rejection = std::addressof(*seasonalRejection);
				}

				Pipeline::CellPipelineDesc pipeline;
				pipeline.snapshot = &snapshot;
				pipeline.lands = lands;
				pipeline.worldEditorID = worldEditorID;
				pipeline.outputDirectory = options.outputDirectory;
				pipeline.fileSuffixes = pass.suffixes;
				pipeline.placement = placement.settings;
				pipeline.blockLayout = Grass::BlockLayout{ .bladesPerBlock = std::addressof(a_shared.grassModels.bladesPerBlock), .capQuadrantBlades = options.capQuadrantBlades };
				pipeline.overwrite = options.overwrite;
				pipeline.skipEmpty = options.skipEmptyCells;
				pipeline.existingFiles = a_shared.existingFiles;
				pipeline.shapesByGrass = &a_shared.shapes.byGrass;
				pipeline.backend = rejection->backend;
				pipeline.world = rejection->index.get();
				pipeline.cpuBvh = rejection->cpuBvh.get();
				pipeline.gpu = rejection->gpu;
				pipeline.validateCpu = options.validateCpu;
				const auto& features = options.rejectionFeatures;
				std::optional<Grass::LandTextureMask> textureMask;
				if (rejection->backend != RejectionBackend::None) {
					pipeline.features = std::addressof(features);
					if (!features.ignoredGrassForms.empty()) {
						pipeline.ignoredGrass = std::addressof(features.ignoredGrassForms);
					}
					if (!features.textureForms.empty()) {
						textureMask.emplace(snapshot, a_worldFormID, features.textureForms);
						pipeline.textureMask = std::addressof(*textureMask);
						pipeline.textureWidth = features.textureWidth;
					}
				}
				pipeline.progress = control.progress ? std::addressof(control.progress->cellsDone) : nullptr;
				pipeline.stop = control.stop;
				pipeline.writer = std::addressof(a_shared.writer);
				pipeline.writeTally = result.tally;
				SetStage(control, RunStage::Generating);
				const auto stats = Pipeline::RunCellPipeline(pipeline);
				LogWorldSummary(a_shared, *rejection, stats, generationBegin);
				Accumulate(result.stats, stats);
				if (stats.cellsCancelled != 0) {
					break;
				}
			}
			return result;
		}

		// The worldspaces a run generates, in order.
		[[nodiscard]] std::vector<GameData::FormID> SelectWorlds(const GenerateOptions& a_options, const GameData::StaticWorldSnapshot& a_snapshot)
		{
			const auto available = ListWorlds(a_snapshot);
			std::vector<GameData::FormID> worlds;
			if (a_options.allWorlds) {
				// NGIO's Only-/Skip-pregenerate-world-spaces, by editor ID: only the first list's when it
				// has any, else all but the second's.
				const auto listed = [](const std::vector<std::string>& a_list, const std::string& a_editorID) {
					return !a_editorID.empty() && std::ranges::any_of(a_list, [&](const std::string& a_name) { return Platform::IEquals(a_name, a_editorID); });
				};
				std::vector<std::string> skipped;
				for (const auto& summary : available) {
					const bool skip = a_options.onlyWorldspaces.empty() ? listed(a_options.skipWorldspaces, summary.editorID)
					                                                    : !listed(a_options.onlyWorldspaces, summary.editorID);
					if (skip) {
						skipped.push_back(summary.editorID.empty() ? std::format("{:08X}", summary.formID.value) : summary.editorID);
					} else {
						worlds.push_back(summary.formID);
					}
				}
				if (worlds.empty()) {
					throw std::runtime_error(skipped.empty() ? "no worldspace has LAND records" : "every worldspace with LAND records is skipped by the NGIO settings");
				}
				spdlog::info("{} worldspace(s) with LAND records{}", worlds.size(),
					skipped.empty() ? std::string{} : std::format("; skipping {} as the NGIO settings ask", skipped.size()));
				for (const auto& name : skipped) {
					spdlog::debug("skipping worldspace {}", name);
				}
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

	RunResult Run(const GenerateOptions& a_requested, const RunControl& a_control)
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
		auto options = a_requested;
		const auto ngio = LoadNgioSettingsFor(options);
		ApplyNgioSettings(ngio, options);
		const auto seasonsSettings = LoadSeasonsSettingsFor(options);
		try {
			SetStage(a_control, RunStage::LoadingPlugins);
			std::optional<LoadedPlugins> loaded;
			if (!a_control.preloaded) {
				loaded.emplace(LoadStaticSnapshot(options));
			}
			const auto& plugins = a_control.preloaded ? *a_control.preloaded : *loaded;
			ThrowIfStopped(a_control);
			if (options.rejection != RejectChoice::None) {
				options.rejectionFeatures = ResolveNgioFeatures(ngio, plugins.loadOrder);
				options.rejectionFeatures.renderGeometry = options.renderGeometry;
				LogRejectionFeatures(options.rejectionFeatures);
			}
			const auto seasons = ResolveSeasons(seasonsSettings, plugins.snapshot, plugins.loadOrder);
			if (seasonsSettings.enabled) {
				(void)CompareAutomaticWinterSwaps(seasonsSettings, seasons, plugins.snapshot, plugins.loadOrder);
			}
			if (!options.dumpSeasonSwapsPath.empty()) {
				if (!seasonsSettings.enabled) {
					throw std::invalid_argument("--dump-season-swaps: Seasons of Skyrim is not installed or not enabled (try --seasons on)");
				}
				DumpSeasonSwaps(seasons, plugins.snapshot, plugins.loadOrder, options.dumpSeasonSwapsPath);
				SetStage(a_control, RunStage::Finished);
				return result;
			}
			if (options.RunsDiagnostic()) {
				result.exitCode = RunDiagnostic(options, plugins);
				SetStage(a_control, RunStage::Finished);
				return result;
			}

			const auto worlds = SelectWorlds(options, plugins.snapshot);
			auto placement = ResolvePlacementSettings(options);
			const auto shapes = MakeQueryShapes(plugins.snapshot, options.rejectionConfig);
			std::optional<Archives::ArchiveResolver> resolver;
			resolver.emplace(MakeResolver(options, plugins));
			const auto grassModels = LoadGrassModels(plugins.snapshot, *resolver);
			placement.unloadableGrass = std::addressof(grassModels.missingGrass);
			ThrowIfStopped(a_control);
			writer.emplace(options.writerThreads, kMaxPendingWriteBytes);
			std::optional<std::unordered_set<std::string>> existingFiles;
			if (options.skipEmptyCells && options.overwrite) {
				existingFiles.emplace(ListExistingCaches(options.outputDirectory));
			}
			RunShared shared{
				.options = options,
				.control = a_control,
				.plugins = plugins,
				.placement = placement,
				.shapes = shapes,
				.begin = begin,
				.writer = *writer,
				.seasons = seasons,
				.existingFiles = existingFiles ? std::addressof(*existingFiles) : nullptr,
				.grassModels = grassModels,
				.resolver = std::move(resolver),
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
				result.cellsEmpty += world.stats.cellsEmpty;
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
					writer->BusySeconds(), options.writerThreads, writer->BusySeconds() * 1.0e6 / static_cast<double>(files), SecondsSince(drainBegin));
			}
			for (const auto& tally : tallies) {
				result.cellsWritten += tally->written.load();
				result.cellsFailed += tally->failed.load();
			}
			if (result.cancelled) {
				spdlog::warn("cancelled after {} worldspace(s): wrote {} file(s)", result.worlds, result.cellsWritten);
			} else {
				spdlog::info("{}wrote {} file(s), skipped {}, empty {}, failed {}, blades={} rejected={} in {:.2f}s",
					result.worlds > 1 ? std::format("all {} worldspace(s): ", result.worlds) : std::string{}, result.cellsWritten, result.cellsSkipped, result.cellsEmpty, result.cellsFailed,
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
