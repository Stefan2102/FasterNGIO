#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace FasterNGIO::Platform
{
	// The launcher's remembered choices: key=value lines (UTF-8), one per setting.
	class UserSettings
	{
	public:
		// %LOCALAPPDATA%\FasterNGIO\settings.ini on Windows; $XDG_CONFIG_HOME (or ~/.config)
		// /fasterngio/settings.ini elsewhere. Empty when no such folder is known.
		[[nodiscard]] static std::filesystem::path DefaultPath();

		// Empty settings when the file does not exist or cannot be read.
		[[nodiscard]] static UserSettings Load(const std::filesystem::path& a_path);
		[[nodiscard]] static UserSettings Parse(std::string_view a_text);

		// Creates the parent folder. False when the file cannot be written.
		bool Save(const std::filesystem::path& a_path) const;
		[[nodiscard]] std::string Serialize() const;

		[[nodiscard]] std::optional<std::string> Get(std::string_view a_key) const;
		void Set(std::string_view a_key, std::string_view a_value);

	private:
		std::map<std::string, std::string, std::less<>> _values;
	};
}
