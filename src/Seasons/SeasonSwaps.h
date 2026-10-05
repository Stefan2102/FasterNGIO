#pragma once

#include "GameData/FormID.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string_view>

namespace FasterNGIO::GameData
{
	struct LoadOrderEntry;
	struct StaticWorldSnapshot;
}

// Seasons of Skyrim's form swaps (powerof3's SeasonsOfSkyrim, FormSwapMap): per season, which land
// texture's grass list and which base objects stand in for others. Grass Cache Helper NG then loads
// "<cell>.<SUFFIX>.cgid" for the current season.
namespace FasterNGIO::Seasons
{
	enum class Season : std::uint8_t
	{
		Winter,
		Spring,
		Summer,
		Autumn
	};
	inline constexpr std::size_t kSeasonCount = 4;
	inline constexpr std::array kSeasons{ Season::Winter, Season::Spring, Season::Summer, Season::Autumn };

	// "Winter", as po3_SeasonsOfSkyrim.ini names the season's section.
	[[nodiscard]] std::string_view SeasonName(Season a_season);
	// "WIN", as the season's swap files end and Grass Cache Helper NG names its caches.
	[[nodiscard]] std::string_view SeasonSuffix(Season a_season);

	// The sections of a swap file that matter for grass caches (VisualEffects never do).
	enum class SwapRecord : std::uint8_t
	{
		LandTextures,
		Activators,
		Furniture,
		MovableStatics,
		Statics,
		Trees,
		Flora
	};
	inline constexpr std::size_t kSwapRecordCount = 7;

	[[nodiscard]] std::string_view SwapRecordSection(SwapRecord a_record);
	// The record type an object section swaps: Seasons picks the map by the original base's type.
	[[nodiscard]] GameData::FourCC SwapRecordSignature(SwapRecord a_record);

	// Original form -> replacement, ordered so maps compare and print deterministically.
	using SwapMap = std::map<GameData::FormID, GameData::FormID>;

	struct SwapMaps
	{
		std::array<SwapMap, kSwapRecordCount> records;

		[[nodiscard]] SwapMap& operator[](SwapRecord a_record) { return records[static_cast<std::size_t>(a_record)]; }
		[[nodiscard]] const SwapMap& operator[](SwapRecord a_record) const { return records[static_cast<std::size_t>(a_record)]; }
		[[nodiscard]] bool Empty() const;

		friend bool operator==(const SwapMaps&, const SwapMaps&) = default;
	};

	// The engine's material ID of a material type (BGSMaterialType::materialID): BSCRC32 of its
	// lower-cased MNAM name.
	[[nodiscard]] std::uint32_t MaterialID(std::string_view a_name);

	// The automatic winter swaps Seasons generates into Data/Seasons/MainFormSwap_WIN.ini whenever
	// the load order changes (FormSwapMap::GenerateFormSwaps): land textures by material, snowy
	// statics, trees, activators, furniture and movable statics. a_editorIDs: powerofthree's Tweaks
	// loads editor IDs, so Seasons' editor-ID blacklists and snow-shader test can match.
	[[nodiscard]] SwapMaps GenerateWinterSwaps(const GameData::StaticWorldSnapshot& a_snapshot, std::span<const GameData::LoadOrderEntry> a_loadOrder,
		bool a_editorIDs);
}
