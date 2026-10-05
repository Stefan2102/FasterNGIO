#pragma once

#include "Options.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace FasterNGIO::Gui
{
	// What the launcher remembers between runs, in Platform::UserSettings' file. Paths are UTF-8, as
	// the text fields edit them; empty means "derived from the game folder".
	struct LauncherSettings
	{
		std::string gameFolder;
		std::string outputFolder;
		std::string pluginsTxt;
		std::string gameIniFolder;
		// Nullopt for every worldspace.
		std::optional<std::uint32_t> world;
		Grass::PlacementMode placement{ Grass::PlacementMode::Smooth };
		App::RejectChoice rejection{ App::RejectChoice::Auto };
		bool overwrite{ false };

		// Defaults for anything missing or unreadable.
		[[nodiscard]] static LauncherSettings Load(const std::filesystem::path& a_path);
		void Save(const std::filesystem::path& a_path) const;
	};
}
