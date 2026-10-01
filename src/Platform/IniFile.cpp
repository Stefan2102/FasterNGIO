#include "Platform/IniFile.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

namespace FasterNGIO::Platform
{
	namespace
	{
		[[nodiscard]] std::string_view Trim(std::string_view a_text)
		{
			const auto first = a_text.find_first_not_of(" \t\r\n");
			if (first == std::string_view::npos) {
				return {};
			}
			const auto last = a_text.find_last_not_of(" \t\r\n");
			return a_text.substr(first, last - first + 1);
		}

		[[nodiscard]] std::string Lower(std::string_view a_text)
		{
			std::string result(a_text);
			std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return result;
		}

		[[nodiscard]] std::string Key(std::string_view a_section, std::string_view a_key)
		{
			return Lower(a_section) + '\n' + Lower(a_key);
		}
	}

	std::optional<IniFile> IniFile::Load(const std::filesystem::path& a_path)
	{
		std::ifstream input(a_path, std::ios::binary);
		if (!input) {
			return std::nullopt;
		}
		const std::string text{ std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
		return Parse(text);
	}

	IniFile IniFile::Parse(std::string_view a_text)
	{
		IniFile result;
		std::string section;
		while (!a_text.empty()) {
			const auto end = a_text.find('\n');
			auto line = Trim(a_text.substr(0, end));
			a_text = end == std::string_view::npos ? std::string_view{} : a_text.substr(end + 1);
			if (line.starts_with("\xEF\xBB\xBF")) {
				line = Trim(line.substr(3));
			}
			if (line.empty() || line.front() == ';' || line.front() == '#') {
				continue;
			}
			if (line.front() == '[') {
				const auto close = line.find(']');
				section = std::string(Trim(line.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1)));
				continue;
			}
			const auto equals = line.find('=');
			if (equals == std::string_view::npos) {
				continue;
			}
			result._values.try_emplace(Key(section, Trim(line.substr(0, equals))), std::string(Trim(line.substr(equals + 1))));
		}
		return result;
	}

	std::optional<std::string> IniFile::Get(std::string_view a_section, std::string_view a_key) const
	{
		const auto it = _values.find(Key(a_section, a_key));
		return it != _values.end() ? std::optional<std::string>(it->second) : std::nullopt;
	}
}
