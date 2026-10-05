#include "Gui/LauncherSettings.h"

#include "Platform/UserSettings.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <ranges>
#include <string_view>
#include <system_error>

namespace FasterNGIO::Gui
{
	LauncherSettings LauncherSettings::Load(const std::filesystem::path& a_path)
	{
		const auto file = Platform::UserSettings::Load(a_path);
		LauncherSettings settings;
		settings.gameFolder = file.Get("game_folder").value_or("");
		settings.outputFolder = file.Get("output_folder").value_or("");
		settings.pluginsTxt = file.Get("plugins_txt").value_or("");
		settings.gameIniFolder = file.Get("game_ini_folder").value_or("");
		settings.placement = App::ParsePlacement(file.Get("placement").value_or("")).value_or(Grass::PlacementMode::Smooth);
		settings.rejection = App::ParseRejectChoice(file.Get("rejection").value_or("")).value_or(App::RejectChoice::Auto);
#if !FASTERNGIO_HAS_GPU
		if (settings.rejection == App::RejectChoice::Gpu) {
			settings.rejection = App::RejectChoice::Auto;
		}
#endif
		settings.overwrite = file.Get("overwrite") == "1";
		settings.renderGeometry = file.Get("render_geometry") == "1";
		// "all", or comma-separated hex form IDs (one, from before the selector took several).
		if (const auto worlds = file.Get("world"); worlds && *worlds != "all") {
			settings.allWorlds = false;
			for (const auto part : std::views::split(std::string_view(*worlds), ',')) {
				std::uint32_t value = 0;
				const std::string_view text(part.begin(), part.end());
				if (std::from_chars(text.data(), text.data() + text.size(), value, 16).ec == std::errc{} && std::ranges::find(settings.worlds, value) == settings.worlds.end()) {
					settings.worlds.push_back(value);
				}
			}
		}
		return settings;
	}

	void LauncherSettings::Save(const std::filesystem::path& a_path) const
	{
		Platform::UserSettings file;
		file.Set("game_folder", gameFolder);
		file.Set("output_folder", outputFolder);
		file.Set("plugins_txt", pluginsTxt);
		file.Set("game_ini_folder", gameIniFolder);
		file.Set("placement", App::PlacementName(placement));
		file.Set("rejection", App::RejectChoiceName(rejection));
		file.Set("overwrite", overwrite ? "1" : "0");
		file.Set("render_geometry", renderGeometry ? "1" : "0");
		std::string worldList;
		for (const auto world : worlds) {
			worldList += std::format("{}{:08X}", worldList.empty() ? "" : ",", world);
		}
		file.Set("world", allWorlds ? std::string("all") : worldList);
		file.Save(a_path);
	}
}
