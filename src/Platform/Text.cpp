#include "Platform/Text.h"

#include <algorithm>
#include <string>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace FasterNGIO::Platform
{
	namespace
	{
		[[nodiscard]] constexpr char ToLower(char a_character)
		{
			return a_character >= 'A' && a_character <= 'Z' ? static_cast<char>(a_character - 'A' + 'a') : a_character;
		}

#if defined(_WIN32)
		// The system setting's ANSI code page, not GetACP(): with the manifest's activeCodePage the
		// process ACP is UTF-8, while the bytes the game text carries follow the system setting.
		[[nodiscard]] UINT SystemAnsiCodePage()
		{
			UINT page = 0;
			if (GetLocaleInfoEx(LOCALE_NAME_SYSTEM_DEFAULT, LOCALE_IDEFAULTCODEPAGE | LOCALE_RETURN_NUMBER, reinterpret_cast<PWSTR>(&page),
				sizeof(page) / sizeof(WCHAR)) == 0) {
				page = CP_UTF8;
			}
			return page;
		}

		[[nodiscard]] std::filesystem::path PathFromWide(std::wstring_view a_text)
		{
			return std::filesystem::path(std::wstring(a_text));
		}
#endif
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

	std::filesystem::path PathFromGameText(std::string_view a_text)
	{
		if (a_text.empty()) {
			return {};
		}
#if defined(_WIN32)
		const auto bytes = static_cast<int>(a_text.size());
		// UTF-8 first (the program's encoding, and what current tools write). MB_ERR_INVALID_CHARS
		// makes the attempt fail, not substitute, so anything older falls through to the system's
		// code page. That second pass drops the error flag: like the game's own narrow file calls,
		// undecodable bytes become the default character instead of raising.
		UINT page = CP_UTF8;
		DWORD flags = MB_ERR_INVALID_CHARS;
		if (MultiByteToWideChar(CP_UTF8, flags, a_text.data(), bytes, nullptr, 0) == 0 && GetLastError() == ERROR_NO_UNICODE_TRANSLATION) {
			page = SystemAnsiCodePage();
			flags = 0;
		}
		const auto wide = MultiByteToWideChar(page, flags, a_text.data(), bytes, nullptr, 0);
		if (wide <= 0) {
			return {};
		}
		std::wstring text(static_cast<std::size_t>(wide), L'\0');
		if (MultiByteToWideChar(page, flags, a_text.data(), bytes, text.data(), wide) != wide) {
			return {};
		}
		return PathFromWide(text);
#else
		// Narrow bytes name files as they are here; there is no code page to get wrong.
		return std::filesystem::path(a_text);
#endif
	}
}
