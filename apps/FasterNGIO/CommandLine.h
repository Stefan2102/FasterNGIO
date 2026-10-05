#pragma once

#include "Options.h"

#include <optional>
#include <span>
#include <string_view>

namespace FasterNGIO::App
{
	// Writes the command-line help to stdout.
	void PrintUsage();

	// The options a_args (without the program name) describe, with the defaults filled in: started
	// from Mod Organizer 2, its instance's game, profile and Data\Grass for whatever is missing.
	// Nullopt for --help. Throws std::invalid_argument for anything invalid.
	[[nodiscard]] std::optional<GenerateOptions> ParseCommandLine(std::span<const std::string_view> a_args);
}
