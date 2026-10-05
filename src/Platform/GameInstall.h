#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace FasterNGIO::Platform
{
	// Which store's build of Skyrim Special Edition an install is. Each keeps plugins.txt and the
	// game INIs in its own folder ("Skyrim Special Edition" plus a suffix).
	enum class GameStore
	{
		Steam,
		Gog,
		Epic,
		MicrosoftStore
	};

	[[nodiscard]] std::string_view GameStoreName(GameStore a_store);

	// The folder name under %LOCALAPPDATA% and Documents\My Games: "Skyrim Special Edition",
	// "... GOG", "... EPIC" or "... MS".
	[[nodiscard]] std::string GameUserFolderName(GameStore a_store);

	// The per-user roots the game's folders live under.
	struct UserFolders
	{
		// %LOCALAPPDATA%: <folder>\plugins.txt.
		std::filesystem::path localAppData;
		// Documents: My Games\<folder>\Skyrim.ini.
		std::filesystem::path documents;
	};

	// This user's folders: the known folders on Windows; elsewhere the Steam Proton prefix under
	// ~/.steam/steam (empty when HOME is unset).
	[[nodiscard]] UserFolders DefaultUserFolders();

	// The Proton prefix's user folders for a Steam library install
	// (<library>/steamapps/common/<game> -> <library>/steamapps/compatdata/489830/pfx/...), or
	// nullopt when a_gameRoot is not in a Steam library.
	[[nodiscard]] std::optional<UserFolders> ProtonUserFolders(const std::filesystem::path& a_gameRoot);

	struct GameInstall
	{
		// The folder holding SkyrimSE.exe.
		std::filesystem::path root;
		std::filesystem::path data;
		GameStore store{ GameStore::Steam };
		// Where the game reads the load order and its INIs; not checked for existence.
		std::filesystem::path pluginsTxt;
		std::filesystem::path iniDirectory;
	};

	// Validates a_folder (the game folder or its Data folder) as a Skyrim Special Edition install
	// and derives the rest from it. The user folders default to the Proton prefix of the install's
	// Steam library off Windows (else DefaultUserFolders()), and to DefaultUserFolders() on Windows.
	// The error is a sentence for the user.
	[[nodiscard]] std::expected<GameInstall, std::string> InspectGameFolder(
		const std::filesystem::path& a_folder,
		const std::optional<UserFolders>& a_userFolders = std::nullopt);
}
