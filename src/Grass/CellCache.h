#pragma once

#include "Grass/NgioCacheWriter.h"
#include "Grass/Placement.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

// From a cell's placed blades to its .cgid file, as NGIO names and lays it out.
namespace FasterNGIO::Grass
{
	// Builds the .cgid payload from the blades that survive. a_rejected is one bit per blade (bit i
	// of word i / 32); an empty span keeps every blade. Groups come out in grass form ID order.
	[[nodiscard]] NgioCellCache FinalizeCell(const CellCandidates& a_candidates, std::span<const std::uint32_t> a_rejected);

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
