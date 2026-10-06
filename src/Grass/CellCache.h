#pragma once

#include "Grass/NgioCacheWriter.h"
#include "Grass/Placement.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

// From a cell's placed blades to its .cgid file, as NGIO names and lays it out.
namespace FasterNGIO::Grass
{
	// The engine's grass blocks, as BSMultiStreamInstanceTriShape builds and loads them. Generating a
	// cell, it collects each type's blades per quadrant in a 256 KiB scratch buffer (AddInstances
	// refuses a batch that would pass 0x3FFFF bytes), then splits them into groups of at most the
	// model's instances per group; each group is one block of the cache. Loading a block reads it into
	// a 256 KiB scratch buffer without checking its size, so a larger block corrupts the heap.
	inline constexpr std::uint32_t kGrassScratchBytes = 0x40000;
	inline constexpr std::uint32_t kBladeBytes = kBladeWords * sizeof(std::uint16_t);
	// The most blades one block may hold (8192).
	inline constexpr std::uint32_t kMaxBladesPerBlock = kGrassScratchBytes / kBladeBytes;
	// The most blades the engine keeps of one type in one quadrant of a cell (8191).
	inline constexpr std::uint32_t kMaxBladesPerQuadrant = (kGrassScratchBytes - 1) / kBladeBytes;

	// The engine's instances per group for a model whose instanced shape has these counts (the
	// renderer keeps each group's 16-bit index range valid: min(0xFFFF / (3 * triangles),
	// 0xFFFF / vertices), from the shape's 16-bit counts), within [1, kMaxBladesPerBlock].
	[[nodiscard]] std::uint32_t BladesPerBlock(std::uint32_t a_triangles, std::uint32_t a_vertices);

	// How FinalizeCell lays out a cell's blocks.
	struct BlockLayout
	{
		// GRAS -> blades per block (GrassModelLayout); a type it lacks, or a null map, uses
		// kMaxBladesPerBlock.
		const std::unordered_map<GameData::FormID, std::uint32_t, GameData::FormIDHash>* bladesPerBlock{ nullptr };
		// Keep at most kMaxBladesPerQuadrant blades of a type per quadrant, as the engine does. The
		// excess is thinned evenly over the quadrant (the engine drops the last batches, which empties
		// the quadrant's last patches).
		bool capQuadrantBlades{ true };
	};

	struct FinalizedCell
	{
		NgioCellCache cache;
		// Blades past the quadrant cap.
		std::uint32_t bladesCapped{ 0 };
	};

	// Builds the .cgid payload from the blades that survive. a_rejected is one bit per blade (bit i
	// of word i / 32); an empty span keeps every blade. Groups come out in grass form ID order, each
	// with its blocks per quadrant in placement order, as the engine writes them.
	[[nodiscard]] FinalizedCell FinalizeCell(const CellCandidates& a_candidates, std::span<const std::uint32_t> a_rejected, const BlockLayout& a_layout = {});

	// "<EditorID>x<XXXX>y<YYYY>.cgid", as NGIO names a cell's cache.
	[[nodiscard]] std::string MakeNgioCacheFileName(std::string_view a_worldEditorID, std::int32_t a_cellX, std::int32_t a_cellY);
	// "<EditorID>x<XXXX>y<YYYY>.<SEASON>.cgid", Grass Cache Helper NG's name for a season's cache
	// (a_season "WIN", "SPR", "SUM" or "AUT"); the plain name when a_season is empty.
	[[nodiscard]] std::string MakeNgioCacheFileName(std::string_view a_worldEditorID, std::int32_t a_cellX, std::int32_t a_cellY, std::string_view a_season);

	// The worldspace's editor ID, "Tamriel" for 0x3C without one, else its form ID in hex.
	[[nodiscard]] std::string ResolveWorldEditorID(const GameData::StaticWorldSnapshot& a_snapshot, GameData::FormID a_worldFormID);

	// Whether a_path holds a plausible cache (a sane group count), so --overwrite is needed to redo it.
	[[nodiscard]] bool ExistingNgioCacheLooksValid(const std::filesystem::path& a_path);
}
