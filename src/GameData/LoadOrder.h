#pragma once

#include "GameData/FormID.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace FasterNGIO::GameData
{
	struct LoadOrderEntry
	{
		std::filesystem::path path;
		// Lower-case file name ("skyrim.esm").
		std::string pluginName;
		ModuleKind kind{ ModuleKind::Full };
		FileID fileID{};
		// Lower-case master file names, as the TES4 header lists them.
		std::vector<std::string> masters{};
		std::uint32_t recordCount{ 0 };
	};

	// The active plugins in load order, as the game builds it: the base game's masters, then
	// Skyrim.ccc's Creation Club plugins, then the lines plugins.txt enables with '*'. Only plugins
	// present in a_dataPath are listed (names match case-insensitively). Throws when plugins.txt
	// cannot be opened.
	[[nodiscard]] std::vector<LoadOrderEntry> ReadPluginsTxt(const std::filesystem::path& a_dataPath, const std::filesystem::path& a_pluginsTxtPath);

	// Reads each plugin's TES4 header (masters, record count, light flag) and assigns the FileIDs.
	[[nodiscard]] std::vector<LoadOrderEntry> PrepareLoadOrder(std::vector<LoadOrderEntry> a_entries);
}
