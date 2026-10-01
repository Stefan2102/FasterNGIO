#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace FasterNGIO::Platform
{
	// The file a_name (a single path component) names in a_directory, matched case-insensitively
	// as Windows and Proton match it. nullopt when there is none.
	[[nodiscard]] std::optional<std::filesystem::path> FindInDirectory(const std::filesystem::path& a_directory, std::string_view a_name);

	// Resolves game resource paths ("meshes\foo\bar.nif") under a Data directory the way the game
	// does: case-insensitively, with either separator. On Windows the filesystem already does
	// this; elsewhere the whole tree is indexed once at construction, so lookups are lock-free
	// and safe from any thread.
	class DataDirectory
	{
	public:
		explicit DataDirectory(std::filesystem::path a_root);

		[[nodiscard]] const std::filesystem::path& Root() const { return _root; }

		// The real path of a regular file, or nullopt.
		[[nodiscard]] std::optional<std::filesystem::path> Find(std::string_view a_relative) const;

	private:
		std::filesystem::path _root;
#if !defined(_WIN32)
		// Lower-case, '/'-separated relative path -> real path.
		std::unordered_map<std::string, std::filesystem::path> _files;
#endif
	};
}
