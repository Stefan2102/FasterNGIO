#include "Platform/DataDirectory.h"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace FasterNGIO::Platform
{
#if !defined(_WIN32)
	namespace
	{
		[[nodiscard]] std::string NormalizeKey(std::string_view a_path)
		{
			std::string key(a_path);
			std::ranges::transform(key, key.begin(), [](unsigned char a_character) {
				return a_character == '\\' ? '/' : static_cast<char>(std::tolower(a_character));
			});
			const auto first = key.find_first_not_of('/');
			return first == std::string::npos ? std::string{} : key.substr(first);
		}
	}
#endif

	std::optional<std::filesystem::path> FindInDirectory(const std::filesystem::path& a_directory, std::string_view a_name)
	{
		std::error_code error;
#if defined(_WIN32)
		auto path = a_directory / std::filesystem::path(std::string(a_name));
		if (std::filesystem::exists(path, error)) {
			return path;
		}
#else
		const auto wanted = NormalizeKey(a_name);
		for (std::filesystem::directory_iterator it(a_directory, std::filesystem::directory_options::skip_permission_denied, error), end; !error && it != end;
			it.increment(error)) {
			if (NormalizeKey(it->path().filename().string()) == wanted) {
				return it->path();
			}
		}
#endif
		return std::nullopt;
	}

	DataDirectory::DataDirectory(std::filesystem::path a_root) :
		_root(std::move(a_root))
	{
#if !defined(_WIN32)
		std::error_code error;
		const auto options = std::filesystem::directory_options::follow_directory_symlink | std::filesystem::directory_options::skip_permission_denied;
		for (std::filesystem::recursive_directory_iterator it(_root, options, error), end; !error && it != end; it.increment(error)) {
			std::error_code typeError;
			if (it->is_regular_file(typeError)) {
				// Two names differing only in case: the game would see one of them; keep the first.
				_files.try_emplace(NormalizeKey(it->path().lexically_relative(_root).string()), it->path());
			}
		}
#endif
	}

	std::optional<std::filesystem::path> DataDirectory::Find(std::string_view a_relative) const
	{
#if defined(_WIN32)
		auto path = _root / std::filesystem::path(std::string(a_relative));
		std::error_code error;
		if (std::filesystem::is_regular_file(path, error)) {
			return path;
		}
		return std::nullopt;
#else
		const auto it = _files.find(NormalizeKey(a_relative));
		return it != _files.end() ? std::optional<std::filesystem::path>(it->second) : std::nullopt;
#endif
	}
}
