#pragma once

#include <string>
#include <string_view>

namespace FasterNGIO::GameData
{
	// A record's model path as the resource key the archives use: trimmed, lower-case,
	// backslash-separated, under "meshes\". Empty when it is not a .nif.
	[[nodiscard]] std::string NormalizeModelPath(std::string_view a_path);
}
