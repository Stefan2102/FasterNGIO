#include "Platform/UserSettings.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#include <Windows.h>
#include <ShlObj.h>
#endif

namespace FasterNGIO::Platform
{
	namespace
	{
		[[nodiscard]] std::string_view Trim(std::string_view a_text)
		{
			const auto first = a_text.find_first_not_of(" \t\r");
			if (first == std::string_view::npos) {
				return {};
			}
			const auto last = a_text.find_last_not_of(" \t\r");
			return a_text.substr(first, last - first + 1);
		}
	}

	std::filesystem::path UserSettings::DefaultPath()
	{
#if defined(_WIN32)
		PWSTR localAppData = nullptr;
		std::filesystem::path result;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)) && localAppData) {
			result = std::filesystem::path(localAppData) / "FasterNGIO" / "settings.ini";
		}
		CoTaskMemFree(localAppData);
		return result;
#else
		if (const char* config = std::getenv("XDG_CONFIG_HOME"); config && *config) {
			return std::filesystem::path(config) / "fasterngio" / "settings.ini";
		}
		if (const char* home = std::getenv("HOME"); home && *home) {
			return std::filesystem::path(home) / ".config" / "fasterngio" / "settings.ini";
		}
		return {};
#endif
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

	std::optional<std::filesystem::path> UserSettings::GetPath(std::string_view a_key) const
	{
		const auto value = Get(a_key);
		if (!value || value->empty()) {
			return std::nullopt;
		}
		return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value->data()), value->size()));
	}

	void UserSettings::SetPath(std::string_view a_key, const std::filesystem::path& a_value)
	{
		const auto text = a_value.u8string();
		Set(a_key, std::string_view(reinterpret_cast<const char*>(text.data()), text.size()));
	}
}
