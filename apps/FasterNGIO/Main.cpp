#include "Archives/ArchiveResolver.h"
#include "Collision/NifCollisionExtractor.h"
#include "Collision/ObjExport.h"
#include "GameData/GameData.h"
#include "Grass/NgioCacheWriter.h"
#include "Grass/Placement.h"
#include "Pipeline/CellPipeline.h"
#include "Rejection/CpuReference.h"
#include "Rejection/RejectionConfig.h"
#include "Rejection/WorldIndex.h"
#if FASTERNGIO_HAS_GPU
#include "Gpu/GpuRejector.h"
#endif

#include <oneapi/tbb/global_control.h>
#include <oneapi/tbb/parallel_for.h>
#include <spdlog/spdlog.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
	using namespace FasterNGIO;

	using Pipeline::RejectionBackend;

	[[nodiscard]] std::filesystem::path ExecutableDirectory()
	{
		wchar_t buffer[MAX_PATH]{};
		GetModuleFileNameW(nullptr, buffer, MAX_PATH);
		return std::filesystem::path(buffer).parent_path();
	}

	struct CliOptions
	{
		std::filesystem::path dataPath;
		std::filesystem::path pluginsTxtPath;
		std::filesystem::path outputDirectory;
		GameData::FormID worldFormID{ 0x0000003Cu };
		std::optional<std::int32_t> singleCellX;
		std::optional<std::int32_t> singleCellY;
		std::optional<std::int32_t> centerCellX;
		std::optional<std::int32_t> centerCellY;
		std::optional<std::int32_t> radius;
		Grass::PlacementSettings placement;
		std::uint32_t threads{ 0 };
		bool explicitGrassPatchSize{ false };
		bool overwrite{ false };
		bool collisionSurvey{ false };
#if FASTERNGIO_HAS_GPU
		RejectionBackend rejection{ RejectionBackend::Gpu };
#else
		RejectionBackend rejection{ RejectionBackend::Cpu };
#endif
		bool validateCpu{ false };
		bool gpuDebugLayer{ false };
		Rejection::RejectionConfig rejectionConfig;
		std::string dumpCollisionModel;
		std::filesystem::path dumpCollisionPath;
	};

	struct LoadedWorld
	{
		std::vector<GameData::LoadOrderEntry> loadOrder;
		GameData::StaticWorldSnapshot snapshot;
	};

	[[nodiscard]] std::filesystem::path DefaultPluginsTxtPath()
	{
		char* localAppData = nullptr;
		std::size_t size = 0;
		std::filesystem::path result;
		if (_dupenv_s(&localAppData, &size, "LOCALAPPDATA") == 0 && localAppData != nullptr && size > 1u && localAppData[0] != '\0') {
			result = std::filesystem::path(localAppData) / "Skyrim Special Edition" / "plugins.txt";
		}
		std::free(localAppData);
		return result;
	}

	[[nodiscard]] std::uint32_t ParseU32(std::string_view a_value)
	{
		std::string text(a_value);
		std::size_t parsed = 0;
		const auto value = std::stoul(text, std::addressof(parsed), 0);
		if (parsed != text.size()) {
			throw std::invalid_argument("invalid integer: " + text);
		}
		return static_cast<std::uint32_t>(value);
	}

	[[nodiscard]] std::int32_t ParseI32(std::string_view a_value)
	{
		std::string text(a_value);
		std::size_t parsed = 0;
		const auto value = std::stol(text, std::addressof(parsed), 0);
		if (parsed != text.size()) {
			throw std::invalid_argument("invalid integer: " + text);
		}
		return static_cast<std::int32_t>(value);
	}

	void PrintUsage()
	{
		std::fputs(
			"Usage: FasterNGIO --data <Skyrim Data> --out <cache dir> [options]\n"
			"\n"
			"Options:\n"
			"  --plugins <plugins.txt>       Defaults to %LOCALAPPDATA%\\Skyrim Special Edition\\plugins.txt\n"
			"  --world <form-id>             Worldspace form ID, default 0x3C (Tamriel)\n"
			"  --cell <x> <y>                Write one cell\n"
			"  --radius <x> <y> <r>          Write cells within a square cell radius\n"
			"  --max-grass-types <n>         Max GRAS records considered per LTEX, default 2\n"
			"  --min-grass-size <n>          iMinGrassSize, default 20\n"
			"  --grass-patch-size <n>        Grass patch radius, default grass-eval-size * 128\n"
			"  --grass-eval-size <n>         Grass eval size, default 2\n"
			"  --alpha-threshold <f>         Landscape alpha threshold, default 0\n"
			"  --water-height <f>            Cell water height fallback\n"
			"  --threads <n>                 Worker thread count, default all cores\n"
			"  --overwrite                   Rebuild cache files that already exist\n"
			"\n"
			"Grass-in-object rejection (NGIO [RayCastConfig] equivalents):\n"
			"  --reject <gpu|cpu|none>       Rejection backend, default gpu\n"
			"  --validate-cpu                With --reject gpu, also run the CPU reference and compare\n"
			"  --gpu-debug                   Enable the D3D12 debug layer\n"
			"  --ray-height <f>              Ray-cast-height, default 150\n"
			"  --ray-depth <f>               Ray-cast-depth, default 5\n"
			"  --ray-mode <0|1>              Ray-cast-mode: 0 ray, 1 capsule (default)\n"
			"  --ray-width <f>               Ray-cast-width, default 0 (size from GRAS bounds)\n"
			"  --ray-width-mult <f>          Ray-cast-width-multiplier, default 0.3\n"
			"\n"
			"Diagnostics:\n"
			"  --collision-survey            Extract the collision of every model the world references and report it\n"
			"  --dump-collision <model> <obj> Write one model's grass-rejecting collision as OBJ\n",
			stdout);
	}

	[[nodiscard]] CliOptions ParseArgs(int argc, char** argv)
	{
		CliOptions options;
		options.pluginsTxtPath = DefaultPluginsTxtPath();

		for (int i = 1; i < argc; ++i) {
			const std::string_view arg = argv[i];
			const auto requireValue = [&](std::string_view a_name) -> std::string_view {
				if (i + 1 >= argc) {
					throw std::invalid_argument(std::format("{} requires a value", a_name));
				}
				return argv[++i];
			};

			if (arg == "--help" || arg == "-h") {
				PrintUsage();
				std::exit(0);
			} else if (arg == "--data") {
				options.dataPath = requireValue(arg);
			} else if (arg == "--plugins") {
				options.pluginsTxtPath = requireValue(arg);
			} else if (arg == "--out") {
				options.outputDirectory = requireValue(arg);
			} else if (arg == "--world") {
				options.worldFormID = GameData::FormID{ ParseU32(requireValue(arg)) };
			} else if (arg == "--cell") {
				options.singleCellX = ParseI32(requireValue(arg));
				options.singleCellY = ParseI32(requireValue(arg));
			} else if (arg == "--radius") {
				options.centerCellX = ParseI32(requireValue(arg));
				options.centerCellY = ParseI32(requireValue(arg));
				options.radius = ParseI32(requireValue(arg));
			} else if (arg == "--max-grass-types") {
				options.placement.maxGrassTypesPerTexture = ParseU32(requireValue(arg));
			} else if (arg == "--min-grass-size") {
				options.placement.minGrassSize = ParseU32(requireValue(arg));
			} else if (arg == "--grass-patch-size") {
				options.placement.grassPatchSize = ParseU32(requireValue(arg));
				options.explicitGrassPatchSize = true;
			} else if (arg == "--grass-eval-size") {
				options.placement.grassEvalSize = ParseU32(requireValue(arg));
				if (!options.explicitGrassPatchSize) {
					options.placement.grassPatchSize = options.placement.grassEvalSize << 7;
				}
			} else if (arg == "--alpha-threshold") {
				options.placement.alphaThreshold = std::stof(std::string(requireValue(arg)));
			} else if (arg == "--water-height") {
				options.placement.waterHeight = std::stof(std::string(requireValue(arg)));
			} else if (arg == "--threads") {
				options.threads = ParseU32(requireValue(arg));
			} else if (arg == "--overwrite") {
				options.overwrite = true;
			} else if (arg == "--reject") {
				const auto value = requireValue(arg);
				if (value == "gpu") {
#if FASTERNGIO_HAS_GPU
					options.rejection = RejectionBackend::Gpu;
#else
					throw std::invalid_argument("this build has no GPU support");
#endif
				} else if (value == "cpu") {
					options.rejection = RejectionBackend::Cpu;
				} else if (value == "none") {
					options.rejection = RejectionBackend::None;
				} else {
					throw std::invalid_argument("expected --reject gpu, cpu or none");
				}
			} else if (arg == "--validate-cpu") {
				options.validateCpu = true;
			} else if (arg == "--gpu-debug") {
				options.gpuDebugLayer = true;
			} else if (arg == "--ray-height") {
				options.rejectionConfig.rayHeight = std::stof(std::string(requireValue(arg)));
			} else if (arg == "--ray-depth") {
				options.rejectionConfig.rayDepth = std::stof(std::string(requireValue(arg)));
			} else if (arg == "--ray-mode") {
				const auto mode = ParseU32(requireValue(arg));
				if (mode > 1) {
					throw std::invalid_argument("--ray-mode 2 (box) is not implemented yet");
				}
				options.rejectionConfig.mode = static_cast<Rejection::QueryMode>(mode);
			} else if (arg == "--ray-width") {
				options.rejectionConfig.rayWidth = std::stof(std::string(requireValue(arg)));
			} else if (arg == "--ray-width-mult") {
				options.rejectionConfig.rayWidthMultiplier = std::stof(std::string(requireValue(arg)));
			} else if (arg == "--collision-survey") {
				options.collisionSurvey = true;
			} else if (arg == "--dump-collision") {
				options.dumpCollisionModel = requireValue(arg);
				options.dumpCollisionPath = requireValue(arg);
			} else {
				throw std::invalid_argument("unknown argument: " + std::string(arg));
			}
		}

		if (options.dataPath.empty()) {
			throw std::invalid_argument("--data is required");
		}
		if (options.pluginsTxtPath.empty()) {
			throw std::invalid_argument("--plugins is required when LOCALAPPDATA is unavailable");
		}
		if (options.outputDirectory.empty() && !options.collisionSurvey && options.dumpCollisionModel.empty()) {
			throw std::invalid_argument("--out is required");
		}
		return options;
	}

	[[nodiscard]] double SecondsSince(std::chrono::steady_clock::time_point a_begin)
	{
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - a_begin).count();
	}

	[[nodiscard]] LoadedWorld LoadStaticSnapshot(const CliOptions& a_options)
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

		std::ranges::sort(noCollision, std::greater{});
		for (std::size_t i = 0; i < (std::min<std::size_t>)(25, noCollision.size()); ++i) {
			spdlog::info("  no collision: {} ({} references)", noCollision[i].second, noCollision[i].first);
		}
		return 0;
	}

	int Run(const CliOptions& a_options)
	{
		if (!a_options.dumpCollisionModel.empty()) {
			return DumpCollision(a_options);
		}
		if (a_options.collisionSurvey) {
			return CollisionSurvey(a_options);
		}
		const auto begin = std::chrono::steady_clock::now();
		const auto world = LoadStaticSnapshot(a_options);
		const auto& snapshot = world.snapshot;
		const auto landsIt = snapshot.landsByWorldspace.find(a_options.worldFormID);
		if (landsIt == snapshot.landsByWorldspace.end() || landsIt->second.empty()) {
			throw std::runtime_error(std::format("no LAND records were loaded for world {:08X}", a_options.worldFormID.value));
		}

		const auto worldEditorID = Grass::ResolveWorldEditorID(snapshot, a_options.worldFormID);
		// The first LAND seen for a cell wins, as in the snapshot's own ordering.
		std::vector<const GameData::LandInfo*> lands;
		std::unordered_set<std::uint64_t> seenCells;
		for (const auto& land : landsIt->second) {
			if (!land.cellX || !land.cellY || !IsSelectedCell(a_options, *land.cellX, *land.cellY)) {
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

		std::filesystem::create_directories(a_options.outputDirectory);
		spdlog::info("world {} ({:08X}): {} cell(s) -> {}", worldEditorID, a_options.worldFormID.value, lands.size(), a_options.outputDirectory.string());

		std::unordered_map<GameData::FormID, Rejection::QueryShape, GameData::FormIDHash> shapesByGrass;
		float maxReach = 0.0f;
		for (const auto& [formID, grass] : snapshot.grassesByFormID) {
			const auto shape = Rejection::MakeQueryShape(a_options.rejectionConfig, grass);
			shapesByGrass.emplace(formID, shape);
			maxReach = (std::max)(maxReach, shape.Reach());
		}

		const auto generationBegin = std::chrono::steady_clock::now();
#if FASTERNGIO_HAS_GPU
		// Device and pipeline creation (shader compile) is the only step that waits on the render
		// thread; the world is then posted and builds while the CPU places grass.
		std::optional<Gpu::GpuRejector> gpu;
		if (a_options.rejection == Pipeline::RejectionBackend::Gpu) {
			float maxRadius = 0.0f;
			for (const auto& [formID, shape] : shapesByGrass) {
				maxRadius = (std::max)(maxRadius, shape.radius);
			}
			const auto exeDirectory = ExecutableDirectory();
			gpu.emplace(Gpu::GpuRejectorDesc{
				.shaderDirectory = exeDirectory / "shaders",
				.shaderCacheDirectory = exeDirectory / "shadercache",
				.mode = a_options.rejectionConfig.mode,
				.segmentLength = a_options.rejectionConfig.rayDepth + a_options.rejectionConfig.rayHeight,
				.maxQueryRadius = maxRadius,
				.debugLayer = a_options.gpuDebugLayer,
			});
		}
#endif

		std::optional<Archives::ArchiveResolver> resolver;
		std::shared_ptr<const Rejection::WorldIndex> worldIndex;
		if (a_options.rejection != Pipeline::RejectionBackend::None) {
			resolver.emplace(MakeResolver(a_options, world));
			worldIndex = std::make_shared<const Rejection::WorldIndex>(snapshot, a_options.worldFormID, *resolver, a_options.rejectionConfig, maxReach);
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
		pipeline.outputDirectory = a_options.outputDirectory;
		pipeline.placement = a_options.placement;
		pipeline.overwrite = a_options.overwrite;
		pipeline.shapesByGrass = &shapesByGrass;
		pipeline.backend = a_options.rejection;
		pipeline.world = worldIndex.get();
#if FASTERNGIO_HAS_GPU
		pipeline.gpu = gpu ? std::addressof(*gpu) : nullptr;
#endif
		pipeline.validateCpu = a_options.validateCpu;
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
		if (a_options.validateCpu) {
			spdlog::info("validation: {} blade(s) differ between GPU and CPU reference", stats.validationMismatches);
		}
		spdlog::info(
			"wrote {} file(s), skipped {}, failed {}, blades={} rejected={} in {:.2f}s (total {:.2f}s)",
			stats.cellsWritten,
			stats.cellsSkipped,
			stats.cellsFailed,
			stats.blades,
			stats.bladesRejected,
			SecondsSince(generationBegin),
			SecondsSince(begin));
		return stats.cellsFailed == 0 ? 0 : 1;
	}
}

int main(int argc, char** argv)
{
	try {
		const auto options = ParseArgs(argc, argv);
		std::optional<oneapi::tbb::global_control> threadLimit;
		if (options.threads > 0) {
			threadLimit.emplace(oneapi::tbb::global_control::max_allowed_parallelism, options.threads);
		}
		return Run(options);
	} catch (const std::exception& e) {
		spdlog::error("{}", e.what());
		return 1;
	}
}
