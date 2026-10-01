#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace FasterNGIO::Platform
{
	// A Windows-style INI file as the game reads it (through the Win32 profile API): [Section]
	// headers, key=value lines, ';' and '#' comment lines. Section and key names compare
	// case-insensitively, and the first occurrence of a key wins.
	class IniFile
	{
	public:
		[[nodiscard]] static std::optional<IniFile> Load(const std::filesystem::path& a_path);
		[[nodiscard]] static IniFile Parse(std::string_view a_text);

		[[nodiscard]] std::optional<std::string> Get(std::string_view a_section, std::string_view a_key) const;

	private:
		// "section\nkey", lower-cased.
		std::unordered_map<std::string, std::string> _values;
	};
}
