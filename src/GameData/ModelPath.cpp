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

	std::string GrassCacheModelPath(std::string_view a_model)
	{
		// The engine keeps MODL as a C string.
		a_model = a_model.substr(0, a_model.find('\0'));
		constexpr std::string_view kPrefix = "meshes\\";
		const auto prefixAt = [&](std::size_t a_at, std::size_t a_length) {
			return a_model.size() - a_at >= a_length && Platform::LowerAscii(a_model.substr(a_at, a_length)) == kPrefix.substr(0, a_length);
		};
		const auto separator = [](char a_c) { return a_c == '/' || a_c == '\\'; };
		// The engine's prefixing of the model path (FUN_140d094f0 with "meshes\"): a path starting with
		// m keeps itself only if it starts with "meshes\"; any other path keeps itself from the first
		// "meshes" between separators. Without either, it gains the prefix. The writer then skips 7
		// characters, so what it stores is the path after that "meshes\" (or the whole MODL).
		if (!a_model.empty() && Platform::LowerAscii(a_model.substr(0, 1)) == "m") {
			return std::string(prefixAt(0, kPrefix.size()) ? a_model.substr(kPrefix.size()) : a_model);
		}
		for (std::size_t i = 0; i + 1 < a_model.size(); ++i) {
			if (separator(a_model[i]) && prefixAt(i + 1, kPrefix.size() - 1) && i + kPrefix.size() < a_model.size() && separator(a_model[i + kPrefix.size()])) {
				return std::string(a_model.substr(i + 1 + kPrefix.size()));
			}
		}
		return std::string(a_model);
	}
}
