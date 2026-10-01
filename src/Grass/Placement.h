#pragma once

#include "GameData/GameData.h"
#include "Grass/NgioCacheWriter.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FasterNGIO::Grass
{
	// Engine grass-manager tunables (INI equivalents). Defaults match vanilla Skyrim SE.
	struct PlacementSettings
	{
		std::uint32_t maxGrassTypesPerTexture{ 2 };
		std::uint32_t grassInstanceStrideWords{ 16 };
		std::uint32_t grassEvalSize{ 2 };
		std::uint32_t minGrassSize{ 20 };
		std::uint32_t grassPatchSize{ grassEvalSize << 7 };
		float alphaThreshold{ 0.0f };
		std::optional<float> waterHeight;
	};

	struct PlacementCounters
	{
		std::uint64_t samplesVisited{ 0 };
		std::uint64_t samplesWithGrassParams{ 0 };
		std::uint64_t grassParamsBuilt{ 0 };
		std::uint64_t latticeCandidates{ 0 };
		std::uint64_t densityRejected{ 0 };
		std::uint64_t waterRejected{ 0 };
		std::uint64_t slopeRejected{ 0 };
		std::uint64_t bladesPlaced{ 0 };
	};

	// One output group (one GRAS) of a cell, in first-use order.
	struct CellGrassGroup
	{
		const GameData::GrassInfo* grass{ nullptr };
		std::string modelPath;
	};

	// A blade exactly as vanilla placement emits it. Rejection only removes blades; it never
	// changes the words of the blades that survive.
	struct BladeCandidate
	{
		std::array<std::uint16_t, 16> words{};
		float position[3]{};
		std::uint32_t groupIndex{ 0 };
	};

	struct CellCandidates
	{
		std::int32_t cellX{ 0 };
		std::int32_t cellY{ 0 };
		std::vector<CellGrassGroup> groups;
		std::vector<BladeCandidate> blades;
		PlacementCounters counters;
	};

	// Runs the engine's grass placement for one exterior LAND, accepting every blade that passes
	// the density, water and slope tests, in the engine's RNG order.
	[[nodiscard]] CellCandidates GenerateCellCandidates(
		const GameData::StaticWorldSnapshot& a_snapshot,
		const GameData::LandInfo& a_land,
		const PlacementSettings& a_settings);

	// Builds the .cgid payload from the blades that survive. a_rejected is one bit per blade
	// (bit i of word i / 32); an empty span keeps every blade.
	[[nodiscard]] NgioCellCache FinalizeCell(
		const CellCandidates& a_candidates,
		std::span<const std::uint32_t> a_rejected,
		std::uint32_t a_strideWords);

	[[nodiscard]] std::string MakeNgioCacheFileName(std::string_view a_worldEditorID, std::int32_t a_cellX, std::int32_t a_cellY);
	[[nodiscard]] std::string ResolveWorldEditorID(const GameData::StaticWorldSnapshot& a_snapshot, GameData::FormID a_worldFormID);
	[[nodiscard]] bool ExistingNgioCacheLooksValid(const std::filesystem::path& a_path);
}
