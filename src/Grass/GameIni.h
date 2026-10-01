#pragma once

#include "Grass/Placement.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace FasterNGIO::Grass
{
	// The folder the game reads Skyrim.ini, SkyrimCustom.ini and SkyrimPrefs.ini from.
	struct GameIniDirectory
	{
		std::filesystem::path path;
		// How it was found, for the log: "--game-ini-dir", "MO2 profile" or "My Games".
		std::string origin;
	};

	// In order: a_explicit; the MO2 profile holding a_pluginsTxt when that profile uses its own game
	// INIs (settings.ini [General] LocalSettings=true); the game's My Games folder (Documents on
	// Windows, which MO2's virtual filesystem also redirects to profile INIs; the Proton prefix
	// elsewhere). Null when none exists.
	[[nodiscard]] std::optional<GameIniDirectory> LocateGameIniDirectory(
		const std::filesystem::path& a_pluginsTxt,
		const std::optional<std::filesystem::path>& a_explicit);

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

	// Reads Skyrim.ini and then SkyrimCustom.ini from a_directory (names matched case-insensitively),
	// the later file overriding, as the engine's Skyrim.ini setting collection does.
	[[nodiscard]] GrassIniSettings ReadGrassIniSettings(const std::filesystem::path& a_directory);

	void ApplyGrassIniSettings(const GrassIniSettings& a_ini, PlacementSettings& a_settings);
}
