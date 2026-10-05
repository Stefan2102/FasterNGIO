#pragma once

#include "GameData/FormID.h"
#include "Gpu/GpuApi.h"
#include "Grass/Placement.h"
#include "Rejection/RejectionConfig.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

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

	struct GenerateOptions
	{
		std::filesystem::path dataPath;
		std::filesystem::path pluginsTxtPath;
		std::filesystem::path outputDirectory;
		GameData::FormID worldFormID{ 0x0000003Cu };
		// Every worldspace with LAND records, one after another, instead of worldFormID.
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
		// Worker threads; 0 for all cores.
		std::uint32_t threads{ 0 };
		// Threads that only write cache files (see Pipeline::FileWriterPool).
		std::uint32_t writerThreads{ 1 };
		bool overwrite{ false };
		RejectChoice rejection{ RejectChoice::Auto };
		Gpu::GpuApi gpuApi{ Gpu::DefaultGpuApi() };
		bool gpuDebugLayer{ false };
		bool validateCpu{ false };
		Rejection::RejectionConfig rejectionConfig;

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
