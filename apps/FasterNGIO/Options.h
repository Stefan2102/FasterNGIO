#pragma once

#include "GameData/FormID.h"
#include "Gpu/GpuApi.h"
#include "Grass/Placement.h"
#include "Rejection/RejectionConfig.h"
#include "Rejection/RejectionFeatures.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What a run generates, as the command line (Main.cpp) and the launcher (Gui/) both describe it.
namespace FasterNGIO::App
{
	enum class RejectChoice
	{
		// The GPU when the adapter supports it, else the CPU BVH.
		Auto,
		Gpu,
		Cpu,
		None
	};

	// Seasons of Skyrim's seasonal caches (SeasonsConfig).
	enum class SeasonsChoice
	{
		// When po3_SeasonsOfSkyrim.dll is installed.
		Auto,
		On,
		Off
	};

	// The ray-cast settings the command line gave; they win over GrassControl.ini.
	struct RejectionOverrides
	{
		std::optional<float> rayHeight;
		std::optional<float> rayDepth;
		std::optional<Rejection::QueryMode> mode;
		std::optional<float> rayWidth;
		std::optional<float> rayWidthMultiplier;
	};

	struct GenerateOptions
	{
		std::filesystem::path dataPath;
		std::filesystem::path pluginsTxtPath;
		std::filesystem::path outputDirectory;
		// The worldspaces to generate, one after another, without repeats; never empty. The
		// diagnostics and --cell/--radius take exactly one.
		std::vector<GameData::FormID> worlds{ GameData::FormID{ 0x0000003Cu } };
		// Every worldspace with LAND records instead of worlds.
		bool allWorlds{ false };
		std::optional<std::int32_t> singleCellX;
		std::optional<std::int32_t> singleCellY;
		std::optional<std::int32_t> centerCellX;
		std::optional<std::int32_t> centerCellY;
		std::optional<std::int32_t> radius;
		// The library defaults to vanilla (engine parity); the tool defaults to smooth.
		Grass::PlacementSettings placement = [] {
			Grass::PlacementSettings settings;
			settings.mode = Grass::PlacementMode::Smooth;
			return settings;
		}();
		// Values for settings the game's INIs also provide; these override the INIs.
		std::optional<std::uint32_t> maxGrassTypesOverride;
		std::optional<std::uint32_t> minGrassSizeOverride;
		std::optional<float> alphaThresholdOverride;
		std::optional<std::filesystem::path> gameIniDirectory;
		bool readGameIni{ true };
		// --grass-patch-size or --grass-eval-size was given, so Super-dense-mode does not set it.
		bool grassPatchSizeChosen{ false };
		// NGIO's GrassControl.ini: nullopt reads Data/SKSE/Plugins/GrassControl.ini when it exists;
		// an empty path reads none.
		std::optional<std::filesystem::path> ngioConfig;
		// Worker threads; 0 for all cores.
		std::uint32_t threads{ 0 };
		// Threads that only write cache files (see Pipeline::FileWriterPool).
		std::uint32_t writerThreads{ 1 };
		bool overwrite{ false };
		// Cells with no grass get no cache file (--write-empty-cells writes NGIO's 4-byte one).
		bool skipEmptyCells{ true };
		// Keep at most the engine's 8191 blades of a grass type per cell quadrant, thinned evenly
		// (Grass::BlockLayout); --no-blade-cap keeps them all.
		bool capQuadrantBlades{ true };
		RejectChoice rejection{ RejectChoice::Auto };
		// --reject (or the launcher) chose it, so NGIO's Ray-cast-enabled does not.
		bool rejectionChosen{ false };
		Gpu::GpuApi gpuApi{ Gpu::DefaultGpuApi() };
		bool gpuDebugLayer{ false };
		bool validateCpu{ false };
		Rejection::RejectionConfig rejectionConfig;
		RejectionOverrides rejectionOverrides;
		// Experimental: objects with rejecting collision reject by their render geometry instead.
		bool renderGeometry{ false };
		SeasonsChoice seasons{ SeasonsChoice::Auto };
		// Writes the resolved season swaps here instead of generating.
		std::filesystem::path dumpSeasonSwapsPath;

		// Filled from GrassControl.ini when a run starts (ApplyNgioSettings, ResolveNgioFeatures).
		// Ensure-max-grass-types-setting: iMaxGrassTypesPerTexure is at least this.
		std::optional<std::uint32_t> ensureMaxGrassTypes;
		// --world all skips these worldspaces (editor IDs), or generates only the second list's.
		std::vector<std::string> skipWorldspaces;
		std::vector<std::string> onlyWorldspaces;
		Rejection::RejectionFeatures rejectionFeatures;

		// Diagnostics; at most one is set, and then no caches are written.
		bool collisionSurvey{ false };
		bool benchmarkRejection{ false };
		std::filesystem::path exportBladesPath;
		std::string dumpCollisionModel;
		std::filesystem::path dumpCollisionPath;

		[[nodiscard]] bool RunsDiagnostic() const { return collisionSurvey || benchmarkRejection || !exportBladesPath.empty() || !dumpCollisionModel.empty(); }
	};

	// The names the command line and the launcher's settings file use.
	[[nodiscard]] std::string_view PlacementName(Grass::PlacementMode a_mode);
	[[nodiscard]] std::optional<Grass::PlacementMode> ParsePlacement(std::string_view a_name);
	[[nodiscard]] std::string_view RejectChoiceName(RejectChoice a_choice);
	// "gpu" parses in every build; whether the build has a GPU path is the caller's check.
	[[nodiscard]] std::optional<RejectChoice> ParseRejectChoice(std::string_view a_name);
}
