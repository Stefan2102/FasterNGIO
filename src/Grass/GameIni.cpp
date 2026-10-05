#include "Grass/GameIni.h"

#include "Platform/DataDirectory.h"
#include "Platform/GameInstall.h"
#include "Platform/IniFile.h"

#include <cstdlib>
#include <system_error>

#if defined(_WIN32)
#include <Windows.h>
#include <ShlObj.h>
#endif

namespace FasterNGIO::Grass
{
	namespace
	{
		[[nodiscard]] bool IsDirectory(const std::filesystem::path& a_path)
		{
			std::error_code error;
			return !a_path.empty() && std::filesystem::is_directory(a_path, error);
		}

		[[nodiscard]] bool IniFlagTrue(const std::optional<std::string>& a_value)
		{
			if (!a_value) {
				return false;
			}
			std::string value;
			for (const auto c : *a_value) {
				value.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
			}
			return value == "true" || value == "1";
		}

		[[nodiscard]] std::filesystem::path DocumentsDirectory([[maybe_unused]] const std::filesystem::path& a_pluginsTxt)
		{
#if defined(_WIN32)
			PWSTR documents = nullptr;
			std::filesystem::path result;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)) && documents) {
				result = documents;
			}
			CoTaskMemFree(documents);
			return result;
#else
			// plugins.txt in a Wine/Proton prefix: <user>/AppData/Local/Skyrim Special Edition/plugins.txt.
			const auto gameData = a_pluginsTxt.parent_path();
			const auto local = gameData.parent_path();
			if (local.filename() == "Local" && local.parent_path().filename() == "AppData") {
				return local.parent_path().parent_path() / "Documents";
			}
			const char* home = std::getenv("HOME");
			if (!home || !*home) {
				return {};
			}
			return std::filesystem::path(home) / ".steam" / "steam" / "steamapps" / "compatdata" / "489830" / "pfx" / "drive_c" / "users" / "steamuser" /
			       "Documents";
#endif
		}

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

	std::optional<GameIniDirectory> LocateGameIniDirectory(const std::filesystem::path& a_pluginsTxt, const std::optional<std::filesystem::path>& a_explicit)
	{
		if (a_explicit) {
			return IsDirectory(*a_explicit) ? std::optional<GameIniDirectory>(GameIniDirectory{ *a_explicit, "--game-ini-dir" }) : std::nullopt;
		}
		const auto profile = a_pluginsTxt.parent_path();
		if (const auto settings = Platform::FindInDirectory(profile, "settings.ini")) {
			if (const auto ini = Platform::IniFile::Load(*settings); ini && IniFlagTrue(ini->Get("General", "LocalSettings"))) {
				return GameIniDirectory{ profile, "MO2 profile" };
			}
		}
		const auto myGames = DocumentsDirectory(a_pluginsTxt) / "My Games";
		for (const auto store : { Platform::GameStore::Steam, Platform::GameStore::Gog, Platform::GameStore::Epic, Platform::GameStore::MicrosoftStore }) {
			if (const auto folder = myGames / Platform::GameUserFolderName(store); IsDirectory(folder)) {
				return GameIniDirectory{ folder, "My Games" };
			}
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
