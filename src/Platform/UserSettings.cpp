#include "Platform/UserSettings.h"

#include "Platform/FileSystem.h"
#include "Platform/Text.h"

#include <fstream>
#include <sstream>
#include <system_error>

namespace FasterNGIO::Platform
{
	std::filesystem::path UserSettings::DefaultPath()
	{
		const auto directory = SettingsDirectory();
		return directory.empty() ? directory : directory / "settings.ini";
	}

	UserSettings UserSettings::Load(const std::filesystem::path& a_path)
	{
		std::ifstream file(a_path, std::ios::binary);
		if (!file) {
			return {};
		}
		std::ostringstream text;
		text << file.rdbuf();
		return Parse(text.str());
	}

	UserSettings UserSettings::Parse(std::string_view a_text)
	{
		UserSettings settings;
		while (!a_text.empty()) {
			const auto end = a_text.find('\n');
			const auto line = Trim(a_text.substr(0, end));
			a_text = end == std::string_view::npos ? std::string_view{} : a_text.substr(end + 1);
			if (line.empty() || line.front() == ';' || line.front() == '#') {
				continue;
			}
			const auto equals = line.find('=');
			if (equals == std::string_view::npos) {
				continue;
			}
			const auto key = Trim(line.substr(0, equals));
			if (!key.empty()) {
				settings._values.try_emplace(std::string(key), std::string(Trim(line.substr(equals + 1))));
			}
		}
		return settings;
	}

	bool UserSettings::Save(const std::filesystem::path& a_path) const
	{
		if (a_path.empty()) {
			return false;
		}
		std::error_code error;
		std::filesystem::create_directories(a_path.parent_path(), error);
		std::ofstream file(a_path, std::ios::binary | std::ios::trunc);
		file << Serialize();
		return static_cast<bool>(file);
	}

	std::string UserSettings::Serialize() const
	{
		std::string text;
		for (const auto& [key, value] : _values) {
			text += key;
			text += '=';
			text += value;
			text += '\n';
		}
		return text;
	}

	std::optional<std::string> UserSettings::Get(std::string_view a_key) const
	{
		const auto it = _values.find(a_key);
		return it == _values.end() ? std::nullopt : std::optional<std::string>(it->second);
	}

	void UserSettings::Set(std::string_view a_key, std::string_view a_value)
	{
		// One line per value: a line break would split it.
		std::string value(a_value);
		std::erase_if(value, [](char c) { return c == '\r' || c == '\n'; });
		_values.insert_or_assign(std::string(a_key), std::move(value));
	}
}
