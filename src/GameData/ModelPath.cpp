#include "GameData/ModelPath.h"

#include "Platform/Text.h"

#include <algorithm>

namespace FasterNGIO::GameData
{
	std::string NormalizeModelPath(std::string_view a_path)
	{
		auto trimmed = Platform::Trim(a_path);
		while (!trimmed.empty() && trimmed.back() == '\0') {
			trimmed.remove_suffix(1);
		}
		auto path = Platform::LowerAscii(Platform::Trim(trimmed));
		std::ranges::replace(path, '/', '\\');
		path.erase(0, path.find_first_not_of('\\'));
		if (!path.ends_with(".nif")) {
			return {};
		}
		if (!path.starts_with("meshes\\")) {
			path = "meshes\\" + path;
		}
		return path;
	}
}
