#include "CommandLine.h"

#include "Platform/GameInstall.h"
#include "Platform/ModOrganizer.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace FasterNGIO::App
{
	namespace
	{
		// The plugins.txt the game in a_data reads: its store's folder under %LOCALAPPDATA% (or the
		// Proton prefix); Steam's when a_data is not a recognisable install.
		[[nodiscard]] std::filesystem::path DefaultPluginsTxtPath(const std::filesystem::path& a_data)
		{
			if (const auto install = Platform::InspectGameFolder(a_data)) {
				return install->pluginsTxt;
			}
			const auto folders = Platform::DefaultUserFolders();
			return folders.localAppData.empty() ? std::filesystem::path{} : folders.localAppData / Platform::GameUserFolderName(Platform::GameStore::Steam) / "plugins.txt";
		}

		// The whole text as a number: decimal, or hexadecimal with 0x (form IDs).
		template <class T>
		[[nodiscard]] T ParseNumber(std::string_view a_value)
		{
			auto text = a_value;
			int base = 10;
			if constexpr (std::is_integral_v<T>) {
				if (text.starts_with("0x") || text.starts_with("0X")) {
					text.remove_prefix(2);
					base = 16;
				}
			}
			T value{};
			std::from_chars_result result{};
			if constexpr (std::is_integral_v<T>) {
				result = std::from_chars(text.data(), text.data() + text.size(), value, base);
			} else {
				result = std::from_chars(text.data(), text.data() + text.size(), value);
			}
			if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
				throw std::invalid_argument(std::format("invalid number: {}", a_value));
			}
			return value;
		}

		[[nodiscard]] std::uint32_t ParseU32(std::string_view a_value) { return ParseNumber<std::uint32_t>(a_value); }
		[[nodiscard]] std::int32_t ParseI32(std::string_view a_value) { return ParseNumber<std::int32_t>(a_value); }
		[[nodiscard]] float ParseFloat(std::string_view a_value) { return ParseNumber<float>(a_value); }
	}

	void PrintUsage()
	{
		std::fputs(
			"Usage: FasterNGIO --data <Skyrim Data> --out <cache dir> [options]\n"
			"       FasterNGIO [--gui]  Open the launcher window (what running it without arguments does)\n"
			"\n"
			"Started from Mod Organizer 2, --data, --plugins and --out default to its game's Data folder, the\n"
			"selected profile's plugins.txt and Data/Grass (new files land in MO2's Overwrite folder).\n"
			"\n"
			"Options:\n"
#if defined(_WIN32)
			"  --plugins <plugins.txt>       Defaults to the game's plugins.txt in %LOCALAPPDATA% (each store has its own)\n"
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

	std::optional<GenerateOptions> ParseCommandLine(std::span<const std::string_view> a_args)
	{
		GenerateOptions options;
		// --grass-eval-size sets the patch size too, unless --grass-patch-size gave one.
		bool explicitGrassPatchSize = false;
		for (std::size_t i = 0; i < a_args.size(); ++i) {
			const auto arg = a_args[i];
			const auto requireValue = [&](std::string_view a_name) -> std::string_view {
				if (i + 1 >= a_args.size()) {
					throw std::invalid_argument(std::format("{} requires a value", a_name));
				}
				return a_args[++i];
			};

			if (arg == "--help" || arg == "-h") {
				return std::nullopt;
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
				options.maxGrassTypesOverride = ParseU32(requireValue(arg));
			} else if (arg == "--min-grass-size") {
				options.minGrassSizeOverride = ParseU32(requireValue(arg));
			} else if (arg == "--grass-patch-size") {
				options.placement.grassPatchSize = ParseU32(requireValue(arg));
				explicitGrassPatchSize = true;
			} else if (arg == "--grass-eval-size") {
				options.placement.grassEvalSize = ParseU32(requireValue(arg));
				if (!explicitGrassPatchSize) {
					options.placement.grassPatchSize = options.placement.grassEvalSize << 7;
				}
			} else if (arg == "--alpha-threshold") {
				options.alphaThresholdOverride = ParseFloat(requireValue(arg));
			} else if (arg == "--water-height") {
				options.placement.waterHeight = ParseFloat(requireValue(arg));
			} else if (arg == "--threads") {
				options.threads = ParseU32(requireValue(arg));
			} else if (arg == "--writers") {
				options.writerThreads = (std::max)(ParseU32(requireValue(arg)), 1u);
			} else if (arg == "--overwrite") {
				options.overwrite = true;
			} else if (arg == "--reject") {
				const auto choice = ParseRejectChoice(requireValue(arg));
				if (!choice) {
					throw std::invalid_argument("expected --reject auto, gpu, cpu or none");
				}
#if !FASTERNGIO_HAS_GPU
				if (*choice == RejectChoice::Gpu) {
					throw std::invalid_argument("this build has no GPU support");
				}
#endif
				options.rejection = *choice;
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
#if FASTERNGIO_HAS_GPU
				options.gpuDebugLayer = true;
#else
				throw std::invalid_argument("this build has no GPU support");
#endif
			} else if (arg == "--ray-height") {
				options.rejectionConfig.rayHeight = ParseFloat(requireValue(arg));
			} else if (arg == "--ray-depth") {
				options.rejectionConfig.rayDepth = ParseFloat(requireValue(arg));
			} else if (arg == "--ray-mode") {
				const auto mode = ParseU32(requireValue(arg));
				if (mode > 1) {
					throw std::invalid_argument("expected --ray-mode 0 (ray) or 1 (capsule); NGIO's box mode (2) is not implemented");
				}
				options.rejectionConfig.mode = static_cast<Rejection::QueryMode>(mode);
			} else if (arg == "--ray-width") {
				options.rejectionConfig.rayWidth = ParseFloat(requireValue(arg));
			} else if (arg == "--ray-width-mult") {
				options.rejectionConfig.rayWidthMultiplier = ParseFloat(requireValue(arg));
			} else if (arg == "--placement") {
				const auto mode = ParsePlacement(requireValue(arg));
				if (!mode) {
					throw std::invalid_argument("expected --placement vanilla or smooth");
				}
				options.placement.mode = *mode;
			} else if (arg == "--smooth-coverage") {
				options.placement.smooth.coverageLow = ParseFloat(requireValue(arg));
				options.placement.smooth.coverageHigh = ParseFloat(requireValue(arg));
			} else if (arg == "--smooth-no-density-match") {
				options.placement.smooth.matchVanillaDensity = false;
			} else if (arg == "--smooth-density-bias") {
				options.placement.smooth.densityBias = ParseFloat(requireValue(arg));
			} else if (arg == "--game-ini-dir") {
				options.gameIniDirectory = std::filesystem::path(requireValue(arg));
			} else if (arg == "--no-game-ini") {
				options.readGameIni = false;
			} else if (arg == "--smooth-warp") {
				options.placement.smooth.warpAmplitude = ParseFloat(requireValue(arg));
				options.placement.smooth.warpWavelength = ParseFloat(requireValue(arg));
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

		const int diagnostics = (options.collisionSurvey ? 1 : 0) + (options.benchmarkRejection ? 1 : 0) + (options.exportBladesPath.empty() ? 0 : 1) +
		                        (options.dumpCollisionModel.empty() ? 0 : 1);
		if (diagnostics > 1) {
			throw std::invalid_argument("choose one of --collision-survey, --benchmark-rejection, --export-blades and --dump-collision");
		}
		if (options.singleCellX && options.radius) {
			throw std::invalid_argument("choose either --cell or --radius");
		}
		if (options.validateCpu && options.rejection == RejectChoice::None) {
			throw std::invalid_argument("--validate-cpu compares rejection results; it needs --reject auto, gpu or cpu");
		}
		if (options.allWorlds) {
			if (options.singleCellX || options.radius) {
				throw std::invalid_argument("--cell and --radius need one worldspace, not --world all");
			}
			if (diagnostics != 0) {
				throw std::invalid_argument("the diagnostics need one worldspace, not --world all");
			}
		}

		// Started from Mod Organizer 2, what is missing comes from its instance and profile. Explicit
		// arguments always win, and nothing is looked up when they are all given.
		const bool needsOutput = !options.RunsDiagnostic();
		if (options.dataPath.empty() || options.pluginsTxtPath.empty() || (needsOutput && options.outputDirectory.empty())) {
			if (const auto mo2 = Platform::DetectModOrganizer(); mo2 && *mo2) {
				const auto& game = (*mo2)->game;
				std::error_code error;
				const bool sameGame = options.dataPath.empty() || std::filesystem::equivalent(options.dataPath, game.data, error);
				if (sameGame) {
					spdlog::info("Mod Organizer 2: {}", (*mo2)->Describe());
					if (options.dataPath.empty()) {
						options.dataPath = game.data;
					}
					if (options.pluginsTxtPath.empty()) {
						options.pluginsTxtPath = game.pluginsTxt;
					}
					if (needsOutput && options.outputDirectory.empty()) {
						options.outputDirectory = game.data / "Grass";
					}
				}
			} else if (mo2) {
				spdlog::warn("started from Mod Organizer 2, but its instance could not be read: {}", mo2->error());
			}
		}
		if (options.dataPath.empty()) {
			throw std::invalid_argument("--data is required (or run FasterNGIO from Mod Organizer 2)");
		}
		if (options.pluginsTxtPath.empty()) {
			options.pluginsTxtPath = DefaultPluginsTxtPath(options.dataPath);
		}
		if (options.pluginsTxtPath.empty()) {
			throw std::invalid_argument("--plugins is required: no default plugins.txt location is known");
		}
		if (needsOutput && options.outputDirectory.empty()) {
			throw std::invalid_argument("--out is required (or run FasterNGIO from Mod Organizer 2)");
		}
		return options;
	}
}
