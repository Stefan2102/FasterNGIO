#include "Platform/FileSystem.h"

#include <cstdlib>
#include <system_error>

#if defined(_WIN32)
#include <Windows.h>
#include <ShlObj.h>
#endif

namespace FasterNGIO::Platform
{
	namespace
	{
#if defined(_WIN32)
		[[nodiscard]] std::filesystem::path KnownFolder(REFKNOWNFOLDERID a_id)
		{
			PWSTR path = nullptr;
			std::filesystem::path result;
			if (SUCCEEDED(SHGetKnownFolderPath(a_id, 0, nullptr, &path)) && path) {
				result = path;
			}
			CoTaskMemFree(path);
			return result;
		}
#else
		// $a_variable/fasterngio, else ~/a_fallback/fasterngio.
		[[nodiscard]] std::filesystem::path XdgDirectory(const char* a_variable, const char* a_fallback)
		{
			if (const char* value = std::getenv(a_variable); value && *value) {
				return std::filesystem::path(value) / "fasterngio";
			}
			if (const char* home = std::getenv("HOME"); home && *home) {
				return std::filesystem::path(home) / a_fallback / "fasterngio";
			}
			return {};
		}
#endif
	}

	bool IsFile(const std::filesystem::path& a_path)
	{
		std::error_code error;
		return !a_path.empty() && std::filesystem::is_regular_file(a_path, error);
	}

	bool IsDirectory(const std::filesystem::path& a_path)
	{
		std::error_code error;
		return !a_path.empty() && std::filesystem::is_directory(a_path, error);
	}

	std::filesystem::path ExecutableDirectory()
	{
#if defined(_WIN32)
		std::wstring buffer(MAX_PATH, L'\0');
		while (true) {
			const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
			if (length < buffer.size()) {
				buffer.resize(length);
				break;
			}
			buffer.resize(buffer.size() * 2);
		}
		return std::filesystem::path(buffer).parent_path();
#else
		std::error_code error;
		return std::filesystem::read_symlink("/proc/self/exe", error).parent_path();
#endif
	}

	std::filesystem::path LocalAppDataDirectory()
	{
#if defined(_WIN32)
		return KnownFolder(FOLDERID_LocalAppData);
#else
		return {};
#endif
	}

	std::filesystem::path DocumentsDirectory()
	{
#if defined(_WIN32)
		return KnownFolder(FOLDERID_Documents);
#else
		return {};
#endif
	}

	std::filesystem::path SettingsDirectory()
	{
#if defined(_WIN32)
		const auto local = LocalAppDataDirectory();
		return local.empty() ? local : local / "FasterNGIO";
#else
		return XdgDirectory("XDG_CONFIG_HOME", ".config");
#endif
	}

	std::filesystem::path CacheDirectory()
	{
#if defined(_WIN32)
		return SettingsDirectory();
#else
		return XdgDirectory("XDG_CACHE_HOME", ".cache");
#endif
	}
}
