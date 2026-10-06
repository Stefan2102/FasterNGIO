#pragma once

#include "Archives/ArchiveResolver.h"
#include "Platform/GameInstall.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace FasterNGIO::Grass
{
	struct PlacementSettings;

	// The folder the game reads Skyrim.ini, SkyrimCustom.ini and SkyrimPrefs.ini from.
	struct GameIniDirectory
	{
		std::filesystem::path path;
		// How it was found, for the log: "--game-ini-dir", "MO2 profile" or "My Games".
		std::string origin;
	};

	// In order: a_explicit; the MO2 profile holding a_pluginsTxt when that profile uses its own game
	// INIs (settings.ini [General] LocalSettings=true); the My Games folder of the install a_data
	// belongs to, named for its store (Documents on Windows, which MO2's virtual filesystem also
	// redirects to profile INIs; the Proton prefix elsewhere; a_userFolders overrides both). Null
	// when none exists.
	[[nodiscard]] std::optional<GameIniDirectory> LocateGameIniDirectory(
		const std::filesystem::path& a_pluginsTxt,
		const std::filesystem::path& a_data,
		const std::optional<std::filesystem::path>& a_explicit,
		const std::optional<Platform::UserFolders>& a_userFolders = std::nullopt);

	template <class T>
	struct GameIniValue
	{
		T value{};
		std::filesystem::path source;
	};

	// The [Grass] settings that change placement, as the game would end up with them.
	struct GrassIniSettings
	{
		std::optional<GameIniValue<std::uint32_t>> minGrassSize;             // iMinGrassSize
		std::optional<GameIniValue<std::uint32_t>> maxGrassTypesPerTexture;  // iMaxGrassTypesPerTexure (sic)
		std::optional<GameIniValue<float>> texturePctThreshold;              // fTexturePctThreshold
		std::vector<std::filesystem::path> filesRead;
	};

	// The plugin INIs the engine reads into its Skyrim.ini collection at startup, in order: for each
	// enabled ('*') line of a_pluginsTxt, Data\<name>.ini, where <name> is the line up to its first
	// ".esp" (else ".esl", else ".esm"), case-insensitively. Only the files that exist. Plugins the
	// game loads implicitly (Skyrim.esm, the DLCs, Creation Club content) are not in plugins.txt and
	// get none, as in the game.
	[[nodiscard]] std::vector<std::filesystem::path> PluginIniFiles(const std::filesystem::path& a_data, const std::filesystem::path& a_pluginsTxt);

	// The files of the engine's Skyrim.ini collection, in the order it reads them: Skyrim.ini and
	// SkyrimCustom.ini from a_iniDirectory (names matched case-insensitively; SkyrimPrefs.ini feeds
	// only the prefs collection), then a_pluginInis. A later file's key overrides an earlier one's.
	[[nodiscard]] std::vector<std::filesystem::path> SkyrimIniFiles(const std::optional<std::filesystem::path>& a_iniDirectory,
		std::span<const std::filesystem::path> a_pluginInis = {});

	// The [Grass] settings from a_files, in order, the later file overriding.
	[[nodiscard]] GrassIniSettings ReadGrassIniSettings(std::span<const std::filesystem::path> a_files);
	// Skyrim.ini and SkyrimCustom.ini from a_directory.
	[[nodiscard]] GrassIniSettings ReadGrassIniSettings(const std::filesystem::path& a_directory);

	void ApplyGrassIniSettings(const GrassIniSettings& a_ini, PlacementSettings& a_settings);

	// [Archive] sResourceArchiveList and sResourceArchiveList2 from the same files, in the same order
	// (the game registers these archives after reading the plugin INIs): the archives the game loads
	// before the plugins' own, which grass models may come from.
	[[nodiscard]] Archives::ArchiveIniLists ReadArchiveIniLists(std::span<const std::filesystem::path> a_files);
	[[nodiscard]] Archives::ArchiveIniLists ReadArchiveIniLists(const std::filesystem::path& a_directory);
}
