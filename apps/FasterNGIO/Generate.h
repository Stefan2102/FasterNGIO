#pragma once

#include "GameData/GameData.h"
#include "Options.h"

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
	// A load order and its snapshot.
	struct LoadedPlugins
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
		const LoadedPlugins* preloaded{ nullptr };
	};

	struct RunResult
	{
		// The process exit code: 0, or 1 when any cell failed or the run was cancelled.
		int exitCode{ 0 };
		bool cancelled{ false };
		std::uint32_t worlds{ 0 };
		std::uint64_t cellsWritten{ 0 };
		std::uint64_t cellsSkipped{ 0 };
		// Cells with no grass, left without a file.
		std::uint64_t cellsEmpty{ 0 };
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
	[[nodiscard]] LoadedPlugins LoadStaticSnapshot(const GenerateOptions& a_options);

	// The worldspaces with LAND records, in form ID order: what --world all generates.
	[[nodiscard]] std::vector<WorldSummary> ListWorlds(const GameData::StaticWorldSnapshot& a_snapshot);

	// Generates the caches (or runs the diagnostic a_options selects). Throws on invalid input.
	RunResult Run(const GenerateOptions& a_options, const RunControl& a_control = {});
}
