#pragma once

#include <filesystem>

namespace FasterNGIO::Platform
{
	// False for an empty path and on any error.
	[[nodiscard]] bool IsFile(const std::filesystem::path& a_path);
	[[nodiscard]] bool IsDirectory(const std::filesystem::path& a_path);

	// The folder holding the running executable.
	[[nodiscard]] std::filesystem::path ExecutableDirectory();

	// %LOCALAPPDATA% and Documents on Windows; empty elsewhere (the game's own folders live in a
	// Proton prefix there: see GameInstall).
	[[nodiscard]] std::filesystem::path LocalAppDataDirectory();
	[[nodiscard]] std::filesystem::path DocumentsDirectory();

	// FasterNGIO's own per-user folders: settings (%LOCALAPPDATA%\FasterNGIO, else
	// $XDG_CONFIG_HOME/fasterngio or ~/.config/fasterngio) and caches (%LOCALAPPDATA%\FasterNGIO,
	// else $XDG_CACHE_HOME/fasterngio or ~/.cache/fasterngio). Empty when none is known.
	[[nodiscard]] std::filesystem::path SettingsDirectory();
	[[nodiscard]] std::filesystem::path CacheDirectory();
}
