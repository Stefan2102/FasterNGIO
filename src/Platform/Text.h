#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace FasterNGIO::Platform
{
	// ASCII-only case folding: game paths, record names and INI keys are ASCII.
	[[nodiscard]] std::string LowerAscii(std::string_view a_text);
	[[nodiscard]] bool IEquals(std::string_view a_lhs, std::string_view a_rhs);

	// a_text without leading and trailing spaces, tabs and line breaks.
	[[nodiscard]] std::string_view Trim(std::string_view a_text);

	// Paths as UTF-8 text (for logs, the UI and settings files) and back.
	[[nodiscard]] std::string Utf8(const std::filesystem::path& a_path);
	[[nodiscard]] std::filesystem::path PathFromUtf8(std::string_view a_text);

	// A path from game text whose encoding the game never declares: record fields, archive entry
	// names and plugins.txt lines are whatever the author's tools wrote, UTF-8 or the system's ANSI
	// code page. Valid UTF-8 decodes as such; anything else decodes through the system's ANSI code
	// page, leniently (invalid bytes become the default character), as the game's own narrow file
	// resolution would. Never throws, whatever the bytes are.
	[[nodiscard]] std::filesystem::path PathFromGameText(std::string_view a_text);
}
