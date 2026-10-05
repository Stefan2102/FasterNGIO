#include "Platform/Text.h"

#include <algorithm>

namespace FasterNGIO::Platform
{
	namespace
	{
		[[nodiscard]] constexpr char ToLower(char a_character)
		{
			return a_character >= 'A' && a_character <= 'Z' ? static_cast<char>(a_character - 'A' + 'a') : a_character;
		}
	}

	std::string LowerAscii(std::string_view a_text)
	{
		std::string result(a_text);
		std::ranges::transform(result, result.begin(), ToLower);
		return result;
	}

	bool IEquals(std::string_view a_lhs, std::string_view a_rhs)
	{
		return std::ranges::equal(a_lhs, a_rhs, [](char a, char b) { return ToLower(a) == ToLower(b); });
	}

	std::string_view Trim(std::string_view a_text)
	{
		constexpr std::string_view kSpace = " \t\r\n";
		const auto first = a_text.find_first_not_of(kSpace);
		if (first == std::string_view::npos) {
			return {};
		}
		return a_text.substr(first, a_text.find_last_not_of(kSpace) - first + 1);
	}

	std::string Utf8(const std::filesystem::path& a_path)
	{
		const auto text = a_path.u8string();
		return std::string(reinterpret_cast<const char*>(text.data()), text.size());
	}

	std::filesystem::path PathFromUtf8(std::string_view a_text)
	{
		return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(a_text.data()), a_text.size()));
	}
}
