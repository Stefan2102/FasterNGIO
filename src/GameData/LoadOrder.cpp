#include "GameData/LoadOrder.h"

#include <tesworld/Plugins.h>
#include "Platform/DataDirectory.h"
#include "Platform/Text.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace FasterNGIO::GameData
{
	namespace
	{
		[[nodiscard]] bool IsPluginName(std::string_view a_name)
		{
			const auto lower = Platform::LowerAscii(a_name);
			return lower.ends_with(".esm") || lower.ends_with(".esp") || lower.ends_with(".esl");
		}
	}

	std::vector<LoadOrderEntry> ReadPluginsTxt(const std::filesystem::path& a_dataPath, const std::filesystem::path& a_pluginsTxtPath)
	{
		std::ifstream input(a_pluginsTxtPath);
		if (!input) {
			throw std::runtime_error("failed to open plugins.txt: " + Platform::Utf8(a_pluginsTxtPath));
		}

		std::vector<LoadOrderEntry> result;
		std::unordered_set<std::string> added;
		const auto appendIfPresent = [&](std::string_view a_pluginName) {
			auto key = Platform::LowerAscii(a_pluginName);
			if (!IsPluginName(key) || added.contains(key)) {
				return;
			}
			// Plugin names are matched case-insensitively, as the game (and Proton) does.
			const auto path = Platform::FindInDirectory(a_dataPath, key);
			if (!path) {
				return;
			}
			result.push_back(LoadOrderEntry{ .path = *path, .pluginName = key });
			added.insert(std::move(key));
		};

		for (const auto* master : { "Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm" }) {
			appendIfPresent(master);
		}

		if (const auto cccPath = Platform::FindInDirectory(a_dataPath.parent_path(), "Skyrim.ccc")) {
			std::ifstream cccInput(*cccPath);
			for (std::string line; std::getline(cccInput, line);) {
				const auto trimmed = Platform::Trim(line);
				if (!trimmed.empty() && !trimmed.starts_with('#')) {
					appendIfPresent(trimmed);
				}
			}
		}

		for (std::string line; std::getline(input, line);) {
			const auto trimmed = Platform::Trim(line);
			// Skyrim SE/AE and MO2 prefix enabled entries with '*'. Unstarred entries remain in
			// plugins.txt to preserve their ordering, but are disabled and must not contribute
			// records to the resolved load order.
			if (!trimmed.starts_with('*')) {
				continue;
			}
			appendIfPresent(Platform::Trim(trimmed.substr(1)));
		}
		return result;
	}

	std::vector<LoadOrderEntry> PrepareLoadOrder(std::vector<LoadOrderEntry> a_entries)
	{
		return tesworld::PrepareLoadOrder(std::move(a_entries));
	}
}
