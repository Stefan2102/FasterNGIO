#pragma once

#include <string>
#include <string_view>

namespace FasterNGIO::GameData
{
	// A record's model path as the resource key the archives use: trimmed, lower-case,
	// backslash-separated, under "meshes\". Empty when it is not a .nif.
	[[nodiscard]] std::string NormalizeModelPath(std::string_view a_path);

	// A GRAS model path as the engine writes it into a .cgid group: the MODL string as stored (case
	// and separators kept) after its "meshes\" prefix, e.g. "Landscape\Grass\DeadPineDrJ03.nif".
	[[nodiscard]] std::string GrassCacheModelPath(std::string_view a_model);
}
