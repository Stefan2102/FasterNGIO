#include "Grass/GameIni.h"

#include "Grass/Placement.h"
#include "Platform/DataDirectory.h"
#include "Platform/FileSystem.h"
#include "Platform/GameInstall.h"
#include "Platform/IniFile.h"
#include "Platform/ModOrganizer.h"

#include <cstdlib>

namespace FasterNGIO::Grass
{
	namespace
	{
		// The game reads values with atoi/atof semantics: the leading number, the rest ignored.
		[[nodiscard]] std::optional<std::uint32_t> ParseInt(const std::string& a_text)
		{
			char* end = nullptr;
			const auto value = std::strtol(a_text.c_str(), &end, 10);
			if (end == a_text.c_str() || value < 0) {
				return std::nullopt;
			}
			return static_cast<std::uint32_t>(value);
		}

		[[nodiscard]] std::optional<float> ParseFloat(const std::string& a_text)
		{
			char* end = nullptr;
			const auto value = std::strtof(a_text.c_str(), &end);
			if (end == a_text.c_str()) {
				return std::nullopt;
			}
			return value;
		}
	}

	std::optional<GameIniDirectory> LocateGameIniDirectory(const std::filesystem::path& a_pluginsTxt, const std::filesystem::path& a_data,
		const std::optional<std::filesystem::path>& a_explicit, const std::optional<Platform::UserFolders>& a_userFolders)
	{
		if (a_explicit) {
			return Platform::IsDirectory(*a_explicit) ? std::optional<GameIniDirectory>(GameIniDirectory{ *a_explicit, "--game-ini-dir" }) : std::nullopt;
		}
		const auto profile = a_pluginsTxt.parent_path();
		if (Platform::Mo2ProfileUsesLocalIni(profile)) {
			return GameIniDirectory{ profile, "MO2 profile" };
		}
		// The install's own store decides the folder name ("Skyrim Special Edition GOG", ...).
		std::filesystem::path myGames;
		if (const auto install = Platform::InspectGameFolder(a_data, a_userFolders)) {
			myGames = install->iniDirectory;
		} else if (const auto folders = a_userFolders.value_or(Platform::DefaultUserFolders()); !folders.documents.empty()) {
			myGames = folders.documents / "My Games" / Platform::GameUserFolderName(Platform::GameStore::Steam);
		}
		if (Platform::IsDirectory(myGames)) {
			return GameIniDirectory{ myGames, "My Games" };
		}
		return std::nullopt;
	}

	GrassIniSettings ReadGrassIniSettings(const std::filesystem::path& a_directory)
	{
		GrassIniSettings result;
		// The grass settings belong to the engine's Skyrim.ini collection, which loads Skyrim.ini and
		// then SkyrimCustom.ini; SkyrimPrefs.ini only feeds the prefs collection.
		for (const auto* name : { "Skyrim.ini", "SkyrimCustom.ini" }) {
			const auto path = Platform::FindInDirectory(a_directory, name);
			if (!path) {
				continue;
			}
			const auto ini = Platform::IniFile::Load(*path);
			if (!ini) {
				continue;
			}
			result.filesRead.push_back(*path);
			if (const auto text = ini->Get("Grass", "iMinGrassSize")) {
				if (const auto value = ParseInt(*text)) {
					result.minGrassSize = GameIniValue<std::uint32_t>{ *value, *path };
				}
			}
			if (const auto text = ini->Get("Grass", "iMaxGrassTypesPerTexure")) {
				if (const auto value = ParseInt(*text)) {
					result.maxGrassTypesPerTexture = GameIniValue<std::uint32_t>{ *value, *path };
				}
			}
			if (const auto text = ini->Get("Grass", "fTexturePctThreshold")) {
				if (const auto value = ParseFloat(*text)) {
					result.texturePctThreshold = GameIniValue<float>{ *value, *path };
				}
			}
		}
		return result;
	}

	void ApplyGrassIniSettings(const GrassIniSettings& a_ini, PlacementSettings& a_settings)
	{
		if (a_ini.minGrassSize) {
			a_settings.minGrassSize = a_ini.minGrassSize->value;
		}
		if (a_ini.maxGrassTypesPerTexture) {
			a_settings.maxGrassTypesPerTexture = a_ini.maxGrassTypesPerTexture->value;
		}
		if (a_ini.texturePctThreshold) {
			a_settings.alphaThreshold = a_ini.texturePctThreshold->value;
		}
	}
}
