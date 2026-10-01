#include "Archives/ArchiveResolver.h"
#include "Collision/NifCollisionExtractor.h"
#include "Collision/ObjExport.h"
#include "GameData/GameData.h"
#include "Grass/NgioCacheWriter.h"
#include "Grass/Placement.h"
#include "Pipeline/CellPipeline.h"
#include "Rejection/CpuBvh.h"
#include "Rejection/CpuReference.h"
#include "Rejection/RejectionConfig.h"
#include "Rejection/WorldIndex.h"
#if FASTERNGIO_HAS_GPU
#include "Gpu/GpuRejector.h"
#endif

#include <oneapi/tbb/global_control.h>
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

namespace
{
	using namespace FasterNGIO;

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

	enum class RejectChoice
	{
		// The GPU when the adapter supports it, else the CPU BVH.
		Auto,
		Gpu,
		Cpu,
		None
	};

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
		bool benchmarkRejection{ false };
		RejectChoice rejection{ RejectChoice::Auto };
		bool validateCpu{ false };
		bool gpuDebugLayer{ false };
#if FASTERNGIO_HAS_GPU
		Gpu::GpuApi gpuApi{ Gpu::DefaultGpuApi() };
#endif
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
#if defined(_WIN32)
		char* localAppData = nullptr;
		std::size_t size = 0;
		std::filesystem::path result;
		if (_dupenv_s(&localAppData, &size, "LOCALAPPDATA") == 0 && localAppData != nullptr && size > 1u && localAppData[0] != '\0') {
			result = std::filesystem::path(localAppData) / "Skyrim Special Edition" / "plugins.txt";
		}
		std::free(localAppData);
		return result;
#else
		// Steam's Proton prefix for Skyrim Special Edition (app 489830).
		const char* home = std::getenv("HOME");
		if (!home || !*home) {
			return {};
		}
		const auto prefix = std::filesystem::path(home) / ".steam" / "steam" / "steamapps" / "compatdata" / "489830" / "pfx";
		return prefix / "drive_c" / "users" / "steamuser" / "AppData" / "Local" / "Skyrim Special Edition" / "plugins.txt";
#endif
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
#if defined(_WIN32)
			"  --plugins <plugins.txt>       Defaults to %LOCALAPPDATA%\\Skyrim Special Edition\\plugins.txt\n"
#else
			"  --plugins <plugins.txt>       Defaults to Skyrim's plugins.txt in the Steam Proton prefix\n"
#endif
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
			"  --reject <auto|gpu|cpu|none>  Rejection backend. auto (default) uses the GPU when it supports\n"
			"                                ray tracing with bindless descriptor heaps, else the CPU BVH\n"
			"  --gpu-api <d3d12|vulkan>      Ray-tracing API, default d3d12 on Windows, vulkan elsewhere\n"
			"  --validate-cpu                Also run the brute-force CPU reference and compare\n"
			"  --gpu-debug                   Enable the D3D12 debug layer / Vulkan validation\n"
			"  --ray-height <f>              Ray-cast-height, default 150\n"
			"  --ray-depth <f>               Ray-cast-depth, default 5\n"
			"  --ray-mode <0|1>              Ray-cast-mode: 0 ray, 1 capsule (default)\n"
			"  --ray-width <f>               Ray-cast-width, default 0 (size from GRAS bounds)\n"
			"  --ray-width-mult <f>          Ray-cast-width-multiplier, default 0.3\n"
			"\n"
			"Diagnostics:\n"
			"  --collision-survey            Extract the collision of every model the world references and report it\n"
			"  --benchmark-rejection         Place every selected cell in memory, then time the CPU BVH (all threads\n"
			"                                and one) and, when available, the GPU on the same queries; writes nothing\n"
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
				if (value == "auto") {
					options.rejection = RejectChoice::Auto;
				} else if (value == "gpu") {
#if FASTERNGIO_HAS_GPU
					options.rejection = RejectChoice::Gpu;
#else
					throw std::invalid_argument("this build has no GPU support");
#endif
				} else if (value == "cpu") {
					options.rejection = RejectChoice::Cpu;
				} else if (value == "none") {
					options.rejection = RejectChoice::None;
				} else {
					throw std::invalid_argument("expected --reject auto, gpu, cpu or none");
				}
			} else if (arg == "--gpu-api") {
				const auto value = requireValue(arg);
#if FASTERNGIO_HAS_GPU
				if (value == "d3d12") {
#if defined(_WIN32)
					options.gpuApi = Gpu::GpuApi::D3D12;
#else
					throw std::invalid_argument("D3D12 is only available on Windows");
#endif
				} else if (value == "vulkan") {
					options.gpuApi = Gpu::GpuApi::Vulkan;
				} else {
					throw std::invalid_argument("expected --gpu-api d3d12 or vulkan");
				}
#else
				(void)value;
				throw std::invalid_argument("this build has no GPU support");
#endif
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
			} else if (arg == "--benchmark-rejection") {
				options.benchmarkRejection = true;
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
			throw std::invalid_argument("--plugins is required: no default plugins.txt location is known");
		}
		if (options.outputDirectory.empty() && !options.collisionSurvey && !options.benchmarkRejection && options.dumpCollisionModel.empty()) {
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


	using ShapeMap = std::unordered_map<GameData::FormID, Rejection::QueryShape, GameData::FormIDHash>;

	// Rejection throughput without placement or file writing in the measurement: every cell is
	// placed up front, then the same queries go through each backend.
	int BenchmarkRejection(const CliOptions& a_options, const GameData::StaticWorldSnapshot& a_snapshot, std::span<const GameData::LandInfo* const> a_lands,
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
			cell.candidates = Grass::GenerateCellCandidates(a_snapshot, *a_lands[i], a_options.placement);
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

		if (!a_options.benchmarkRejection) {
			std::filesystem::create_directories(a_options.outputDirectory);
		}
		spdlog::info("world {} ({:08X}): {} cell(s) -> {}", worldEditorID, a_options.worldFormID.value, lands.size(), a_options.outputDirectory.string());

		ShapeMap shapesByGrass;
		float maxReach = 0.0f;
		for (const auto& [formID, grass] : snapshot.grassesByFormID) {
			const auto shape = Rejection::MakeQueryShape(a_options.rejectionConfig, grass);
			shapesByGrass.emplace(formID, shape);
			maxReach = (std::max)(maxReach, shape.Reach());
		}

		const auto generationBegin = std::chrono::steady_clock::now();
		auto backend = a_options.rejection == RejectChoice::None ? RejectionBackend::None : RejectionBackend::Cpu;
#if FASTERNGIO_HAS_GPU
		// Device and pipeline creation (shader compile) is the only step that waits on the render
		// thread; the world is then posted and builds while the CPU places grass.
		std::optional<Gpu::GpuRejector> gpu;
		if (a_options.rejection == RejectChoice::Auto || a_options.rejection == RejectChoice::Gpu) {
			float maxRadius = 0.0f;
			for (const auto& [formID, shape] : shapesByGrass) {
				maxRadius = (std::max)(maxRadius, shape.radius);
			}
			const auto exeDirectory = ExecutableDirectory();
			try {
				gpu.emplace(Gpu::GpuRejectorDesc{
					.api = a_options.gpuApi,
					.shaderDirectory = exeDirectory / "shaders",
					.shaderCacheDirectory = exeDirectory / "shadercache",
					.mode = a_options.rejectionConfig.mode,
					.segmentLength = a_options.rejectionConfig.rayDepth + a_options.rejectionConfig.rayHeight,
					.maxQueryRadius = maxRadius,
					.debugLayer = a_options.gpuDebugLayer,
				});
				backend = RejectionBackend::Gpu;
			} catch (const std::exception& e) {
				if (a_options.rejection == RejectChoice::Gpu) {
					throw;
				}
				spdlog::warn("GPU rejection unavailable, using the CPU BVH: {}", e.what());
			}
		}
#endif
		spdlog::info("rejection: {}", backend == RejectionBackend::Gpu ? "GPU" : backend == RejectionBackend::Cpu ? "CPU BVH" : "off");

		std::optional<Archives::ArchiveResolver> resolver;
		std::shared_ptr<const Rejection::WorldIndex> worldIndex;
		std::optional<Rejection::CpuBvh> cpuBvh;
		if (backend != RejectionBackend::None) {
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
		if (a_options.benchmarkRejection) {
			if (!worldIndex) {
				throw std::invalid_argument("--benchmark-rejection needs rejection enabled");
			}
#if FASTERNGIO_HAS_GPU
			return BenchmarkRejection(a_options, snapshot, lands, shapesByGrass, worldIndex, gpu ? std::addressof(*gpu) : nullptr);
#else
			return BenchmarkRejection(a_options, snapshot, lands, shapesByGrass, worldIndex, nullptr);
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
		pipeline.backend = backend;
		pipeline.world = worldIndex.get();
		pipeline.cpuBvh = cpuBvh ? std::addressof(*cpuBvh) : nullptr;
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
			spdlog::info("validation: {} blade(s) differ from the brute-force CPU reference", stats.validationMismatches);
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
