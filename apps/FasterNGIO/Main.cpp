#include "Generate.h"
#include "Gui/Launcher.h"
#include "Platform/GameInstall.h"

#include <oneapi/tbb/global_control.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
	using namespace FasterNGIO;

	using App::CliOptions;
	using App::RejectChoice;

	[[nodiscard]] std::filesystem::path DefaultPluginsTxtPath()
	{
		// Steam's: %LOCALAPPDATA% on Windows, the Proton prefix elsewhere.
		const auto folders = Platform::DefaultUserFolders();
		return folders.localAppData.empty() ? std::filesystem::path{} : folders.localAppData / Platform::GameUserFolderName(Platform::GameStore::Steam) / "plugins.txt";
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
			"       FasterNGIO [--gui]  Open the launcher window (what running it without arguments does)\n"
			"\n"
			"Options:\n"
#if defined(_WIN32)
			"  --plugins <plugins.txt>       Defaults to %LOCALAPPDATA%\\Skyrim Special Edition\\plugins.txt\n"
#else
			"  --plugins <plugins.txt>       Defaults to Skyrim's plugins.txt in the Steam Proton prefix\n"
#endif
			"  --world <form-id|all>         Worldspace form ID, default 0x3C (Tamriel); all: every worldspace\n"
			"                                with LAND records, as NGIO's pregeneration does\n"
			"  --cell <x> <y>                Write one cell\n"
			"  --radius <x> <y> <r>          Write cells within a square cell radius\n"
			"  --max-grass-types <n>         iMaxGrassTypesPerTexure (the engine takes n + 1 per LTEX), default 2\n"
			"  --min-grass-size <n>          iMinGrassSize, default 20\n"
			"  --grass-patch-size <n>        Grass patch radius, default grass-eval-size * 128\n"
			"  --grass-eval-size <n>         Grass eval size, default 2\n"
			"  --alpha-threshold <f>         fTexturePctThreshold, default 0\n"
			"  --water-height <f>            Cell water height fallback\n"
			"  --placement <smooth|vanilla>  smooth (default) evaluates a continuous, seam-free texture-weight\n"
			"                                field at every blade; vanilla reproduces the engine\n"
			"  --smooth-coverage <lo> <hi>   Smooth: texture weight ramp from no grass to full density (0.05 0.5)\n"
			"  --smooth-density-bias <f>     Smooth: coverage multiplier before the cap at full density, shifting\n"
			"                                grass from texture cores into blends (1.6; 1 is proportional to weight)\n"
			"  --smooth-no-density-match     Smooth: do not scale each grass type to vanilla's worldspace total\n"
			"  --smooth-warp <amp> <wave>    Smooth: domain warp amplitude and wavelength in units (96 512; 0 off)\n"
			"  --game-ini-dir <dir>          Read the [Grass] settings from this folder's Skyrim.ini/SkyrimCustom.ini.\n"
			"                                Default: the MO2 profile (if it uses local INIs), else My Games\n"
			"  --no-game-ini                 Ignore the game's INIs; use engine defaults and command-line values\n"
			"  --threads <n>                 Worker thread count, default all cores\n"
			"  --writers <n>                 Threads that only write cache files, default 1 (file creation is\n"
			"                                serialized by NTFS and antivirus scanning; faster filesystems may take more)\n"
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
			"  --export-blades <file>        Write every selected cell's placed blades (x, y: float32, grass form ID:\n"
			"                                uint32) before rejection, for offline analysis; writes no caches\n"
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
				if (const auto value = requireValue(arg); value == "all") {
					options.allWorlds = true;
				} else {
					options.allWorlds = false;
					options.worldFormID = GameData::FormID{ ParseU32(value) };
				}
			} else if (arg == "--cell") {
				options.singleCellX = ParseI32(requireValue(arg));
				options.singleCellY = ParseI32(requireValue(arg));
			} else if (arg == "--radius") {
				options.centerCellX = ParseI32(requireValue(arg));
				options.centerCellY = ParseI32(requireValue(arg));
				options.radius = ParseI32(requireValue(arg));
			} else if (arg == "--max-grass-types") {
				options.cliMaxGrassTypes = ParseU32(requireValue(arg));
			} else if (arg == "--min-grass-size") {
				options.cliMinGrassSize = ParseU32(requireValue(arg));
			} else if (arg == "--grass-patch-size") {
				options.placement.grassPatchSize = ParseU32(requireValue(arg));
				options.explicitGrassPatchSize = true;
			} else if (arg == "--grass-eval-size") {
				options.placement.grassEvalSize = ParseU32(requireValue(arg));
				if (!options.explicitGrassPatchSize) {
					options.placement.grassPatchSize = options.placement.grassEvalSize << 7;
				}
			} else if (arg == "--alpha-threshold") {
				options.cliAlphaThreshold = std::stof(std::string(requireValue(arg)));
			} else if (arg == "--water-height") {
				options.placement.waterHeight = std::stof(std::string(requireValue(arg)));
			} else if (arg == "--threads") {
				options.threads = ParseU32(requireValue(arg));
			} else if (arg == "--writers") {
				options.writerThreads = (std::max)(ParseU32(requireValue(arg)), 1u);
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
			} else if (arg == "--placement") {
				const auto value = requireValue(arg);
				if (value == "vanilla") {
					options.placement.mode = Grass::PlacementMode::Vanilla;
				} else if (value == "smooth") {
					options.placement.mode = Grass::PlacementMode::Smooth;
				} else {
					throw std::invalid_argument("expected --placement vanilla or smooth");
				}
			} else if (arg == "--smooth-coverage") {
				options.placement.smooth.coverageLow = std::stof(std::string(requireValue(arg)));
				options.placement.smooth.coverageHigh = std::stof(std::string(requireValue(arg)));
			} else if (arg == "--smooth-no-density-match") {
				options.placement.smooth.matchVanillaDensity = false;
			} else if (arg == "--smooth-density-bias") {
				options.placement.smooth.densityBias = std::stof(std::string(requireValue(arg)));
			} else if (arg == "--game-ini-dir") {
				options.gameIniDirectory = std::filesystem::path(requireValue(arg));
			} else if (arg == "--no-game-ini") {
				options.readGameIni = false;
			} else if (arg == "--smooth-warp") {
				options.placement.smooth.warpAmplitude = std::stof(std::string(requireValue(arg)));
				options.placement.smooth.warpWavelength = std::stof(std::string(requireValue(arg)));
			} else if (arg == "--export-blades") {
				options.exportBladesPath = requireValue(arg);
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

		if (options.allWorlds) {
			if (options.singleCellX || options.radius) {
				throw std::invalid_argument("--cell and --radius need one worldspace, not --world all");
			}
			if (options.collisionSurvey || options.benchmarkRejection || !options.exportBladesPath.empty() || !options.dumpCollisionModel.empty()) {
				throw std::invalid_argument("the diagnostics need one worldspace, not --world all");
			}
		}
		if (options.dataPath.empty()) {
			throw std::invalid_argument("--data is required");
		}
		if (options.pluginsTxtPath.empty()) {
			throw std::invalid_argument("--plugins is required: no default plugins.txt location is known");
		}
		if (options.outputDirectory.empty() && !options.collisionSurvey && !options.benchmarkRejection && options.exportBladesPath.empty() &&
			options.dumpCollisionModel.empty()) {
			throw std::invalid_argument("--out is required");
		}
		return options;
	}
}

int main(int argc, char** argv)
{
	// No arguments (a double-click) or --gui: the launcher window. Any other arguments, as Mod
	// Organizer 2 passes them, run the command line unchanged.
	if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--gui")) {
		return Gui::RunLauncher();
	}
	try {
		const auto options = ParseArgs(argc, argv);
		std::optional<oneapi::tbb::global_control> threadLimit;
		if (options.threads > 0) {
			// The main thread only waits while the pipeline runs, so it does not count as a worker: a limit
			// of n alone would leave --threads 1 with no thread to run anything.
			threadLimit.emplace(oneapi::tbb::global_control::max_allowed_parallelism, options.threads + 1);
		}
		return App::Run(options).exitCode;
	} catch (const std::exception& e) {
		spdlog::error("{}", e.what());
		return 1;
	}
}
