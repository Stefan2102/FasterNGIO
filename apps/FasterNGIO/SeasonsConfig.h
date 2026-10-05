#pragma once

#include "Options.h"
#include "Seasons/SeasonSwaps.h"

#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace FasterNGIO::GameData
{
	struct LoadOrderEntry;
	struct StaticWorldSnapshot;
}

// Seasons of Skyrim support: which seasons get their own grass caches (Grass Cache Helper NG loads
// "<cell>.<SUFFIX>.cgid" for the current season) and the swaps each applies, read as Seasons 1.9
// reads po3_SeasonsOfSkyrim.ini and Data/Seasons/*_<SUFFIX>.ini.
namespace FasterNGIO::App
{
	// One "base|swap" entry of a swap file, as written: "0xID~Plugin.esp" or an editor ID.
	struct SeasonSwapEntry
	{
		std::string base;
		std::string swap;
	};

	// A Data/Seasons/*_<SUFFIX>.ini file.
	struct SeasonSwapFile
	{
		std::filesystem::path path;
		Seasons::Season season{ Seasons::Season::Winter };
		std::array<std::vector<SeasonSwapEntry>, Seasons::kSwapRecordCount> entries;
		// [Worldspaces] keys.
		std::vector<std::string> worldspaces;
	};

	// One season's po3_SeasonsOfSkyrim.ini section.
	struct SeasonConfig
	{
		std::vector<std::string> worldspaces{ "Tamriel", "MarkarthWorld", "RiftenWorld", "SolitudeWorld", "WhiterunWorld", "DLC1HunterHQWorld",
			"DLC2SolstheimWorld" };
		// Seasons' "Grass": seasonal grass types.
		bool swapGrass{ true };
		// Grass Cache Helper NG's reading of the same key: only "true" loads the season's own caches.
		bool seasonalCaches{ true };
		// Activators, Furniture, Movable Statics, Statics, Trees, Flora, indexed by SwapRecord
		// (LandTextures unused).
		std::array<bool, Seasons::kSwapRecordCount> swapObjects{ true, true, true, true, true, true, true };
	};

	struct SeasonsSettings
	{
		// po3_SeasonsOfSkyrim.dll is in Data/SKSE/Plugins.
		bool installed{ false };
		bool cacheHelperInstalled{ false };
		// powerofthree's Tweaks with Load EditorIDs, which Seasons' editor-ID matching depends on.
		bool tweaksEditorIDs{ false };
		// Seasons will be generated (the choice, detection and Season Type allow it).
		bool enabled{ false };
		std::filesystem::path settingsFile;
		bool settingsFound{ false };
		// 0 disabled, 1-4 a permanent season, 5 seasonal.
		std::uint32_t seasonType{ 5 };
		std::array<SeasonConfig, Seasons::kSeasonCount> seasons{};
		// [Winter]: ignore the automatic winter swaps, or some of their sections.
		bool ignoreAutomaticWinter{ false };
		std::array<bool, Seasons::kSwapRecordCount> skipAutomaticWinter{};
		// Data/Seasons/MainFormSwap_WIN.ini, Seasons' own copy of the automatic winter swaps.
		std::filesystem::path automaticWinterFile;
		std::vector<SeasonSwapFile> swapFiles;
	};

	// A season resolved against the load order.
	struct ResolvedSeason
	{
		Seasons::Season season{ Seasons::Season::Winter };
		SeasonConfig config;
		Seasons::SwapMaps maps;
	};

	struct ResolvedSeasons
	{
		// The seasons that get their own caches, in Seasons' order (WIN, SPR, SUM, AUT).
		std::vector<ResolvedSeason> seasons;
		// The automatic winter swaps before the swap files, for --dump-season-swaps and the
		// comparison with MainFormSwap_WIN.ini.
		Seasons::SwapMaps automaticWinter;
	};

	using FormSwaps = std::unordered_map<GameData::FormID, GameData::FormID, GameData::FormIDHash>;

	// One generation pass of a world: the swaps it applies and the file names it writes, "" for the
	// plain cache and "WIN" etc. for seasons whose swaps are the same.
	struct SeasonPass
	{
		std::vector<std::string> suffixes;
		// Land texture -> the land texture whose grass list it takes.
		FormSwaps landTextureGrass;
		// Reference base object -> the base object Seasons places instead.
		FormSwaps baseObjects;
	};

	// The parsers, for tests.
	void ParseSeasonsIni(std::string_view a_text, SeasonsSettings& a_settings);
	void ParseSeasonSwapFile(std::string_view a_text, SeasonSwapFile& a_file);

	// Detects Seasons, Grass Cache Helper NG and Tweaks under a_data, and reads their settings and the
	// swap files. Not enabled when a_choice is Off, or Auto without Seasons (or with Season Type 0).
	[[nodiscard]] SeasonsSettings ReadSeasonsSettings(const std::filesystem::path& a_data, SeasonsChoice a_choice);

	// ReadSeasonsSettings for a run, logged.
	[[nodiscard]] SeasonsSettings LoadSeasonsSettingsFor(const GenerateOptions& a_options);

	// The enabled seasons' swaps: the automatic winter swaps (unless ignored), then each swap file in
	// path order, later entries winning. Entries that do not resolve are logged and dropped.
	[[nodiscard]] ResolvedSeasons ResolveSeasons(const SeasonsSettings& a_settings, const GameData::StaticWorldSnapshot& a_snapshot,
		std::span<const GameData::LoadOrderEntry> a_loadOrder);

	// A world's passes: the plain pass (with every season whose swaps do not apply in this world),
	// then one per distinct set of seasonal swaps. Without seasons, the plain pass alone. Object swaps
	// only matter to rejection, so a_objectSwaps false drops them.
	[[nodiscard]] std::vector<SeasonPass> PlanSeasonPasses(const ResolvedSeasons& a_seasons, const GameData::StaticWorldSnapshot& a_snapshot,
		std::string_view a_worldEditorID, bool a_objectSwaps);

	// Writes every season's resolved swaps in Seasons' own file syntax, by section.
	void DumpSeasonSwaps(const ResolvedSeasons& a_seasons, const GameData::StaticWorldSnapshot& a_snapshot, std::span<const GameData::LoadOrderEntry> a_loadOrder,
		const std::filesystem::path& a_path);

	// Compares the generated automatic winter swaps with Seasons' MainFormSwap_WIN.ini when it
	// exists, logging what differs; the number of differing entries.
	std::size_t CompareAutomaticWinterSwaps(const SeasonsSettings& a_settings, const ResolvedSeasons& a_seasons,
		const GameData::StaticWorldSnapshot& a_snapshot, std::span<const GameData::LoadOrderEntry> a_loadOrder);

	// One line for the launcher.
	[[nodiscard]] std::string DescribeSeasons(const SeasonsSettings& a_settings);
}
