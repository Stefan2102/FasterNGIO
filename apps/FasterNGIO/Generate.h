#pragma once

#include "GameData/GameData.h"
#include "Grass/Placement.h"
#include "Rejection/RejectionConfig.h"
#if FASTERNGIO_HAS_GPU
#include "Gpu/GpuRejector.h"
#endif

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

// Cache generation, shared by the command line (Main.cpp) and the launcher window (Gui/).
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

	struct CliOptions
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
		// Command-line values for settings the game's INIs also provide; these override the INIs.
		std::optional<std::uint32_t> cliMaxGrassTypes;
		std::optional<std::uint32_t> cliMinGrassSize;
		std::optional<float> cliAlphaThreshold;
		std::optional<std::filesystem::path> gameIniDirectory;
		bool readGameIni{ true };
		std::uint32_t threads{ 0 };
		// Threads that only write cache files (see Pipeline::CacheWriter).
		std::uint32_t writerThreads{ 1 };
		bool explicitGrassPatchSize{ false };
		bool overwrite{ false };
		bool collisionSurvey{ false };
		bool benchmarkRejection{ false };
		std::filesystem::path exportBladesPath;
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

	enum class RunStage : std::uint32_t
	{
		LoadingPlugins,
		// The smooth weight field, collision extraction and the rejection structures of one world.
		Preparing,
		Generating,
		// Every cell is placed; the writer threads are finishing the last files.
		Writing,
		Finished
	};

	// Written by the run, read by anyone (the launcher polls it every frame).
	struct RunProgress
	{
		std::atomic<RunStage> stage{ RunStage::LoadingPlugins };
		std::atomic<std::uint32_t> worldIndex{ 0 };
		std::atomic<std::uint32_t> worldCount{ 0 };
		std::atomic<std::uint32_t> worldFormID{ 0 };
		std::atomic<std::uint32_t> cellsDone{ 0 };
		std::atomic<std::uint32_t> cellsTotal{ 0 };
	};

	struct RunControl
	{
		// Checked between stages and before each cell; a stopped run ends with cancelled set.
		std::stop_token stop;
		RunProgress* progress{ nullptr };
		// A snapshot already loaded from the same data path and plugins.txt, to skip loading it again.
		const LoadedWorld* preloaded{ nullptr };
	};

	struct RunResult
	{
		// The process exit code: 0, or 1 when any cell failed.
		int exitCode{ 0 };
		bool cancelled{ false };
		std::uint32_t worlds{ 0 };
		std::uint64_t cellsWritten{ 0 };
		std::uint64_t cellsSkipped{ 0 };
		std::uint64_t cellsFailed{ 0 };
	};

	struct WorldSummary
	{
		GameData::FormID formID;
		std::string editorID;
		std::size_t cells{ 0 };
	};

	// Parses every plugin in the load order into the static world snapshot. Throws on a missing
	// plugins.txt or an unreadable plugin.
	[[nodiscard]] LoadedWorld LoadStaticSnapshot(const CliOptions& a_options);

	// The worldspaces with LAND records, in form ID order: what --world all generates.
	[[nodiscard]] std::vector<WorldSummary> ListWorlds(const GameData::StaticWorldSnapshot& a_snapshot);

	// Generates the caches (or runs the diagnostic a_options selects). Throws on invalid input.
	RunResult Run(const CliOptions& a_options, const RunControl& a_control = {});
}
