#include "Gui/LauncherSettings.h"

#include "Platform/UserSettings.h"

#include <charconv>
#include <format>
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
		if (const auto world = file.Get("world"); world && *world != "all") {
			std::uint32_t value = 0;
			if (std::from_chars(world->data(), world->data() + world->size(), value, 16).ec == std::errc{}) {
				settings.world = value;
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
		file.Set("world", world ? std::format("{:08X}", *world) : std::string("all"));
		file.Save(a_path);
	}
}
