#pragma once

#include "Options.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

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
		// Every worldspace, or the form IDs in worlds (which may be empty, or name worldspaces another
		// load order has; the launcher only runs the ones the current load order has).
		bool allWorlds{ true };
		std::vector<std::uint32_t> worlds;
		Grass::PlacementMode placement{ Grass::PlacementMode::Smooth };
		App::RejectChoice rejection{ App::RejectChoice::Auto };
		bool overwrite{ false };
		// Experimental: GenerateOptions::renderGeometry.
		bool renderGeometry{ false };

		// Defaults for anything missing or unreadable.
		[[nodiscard]] static LauncherSettings Load(const std::filesystem::path& a_path);
		void Save(const std::filesystem::path& a_path) const;
	};
}
