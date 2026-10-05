#include "Platform/ModOrganizer.h"

#include "Platform/DataDirectory.h"
#include "Platform/FileSystem.h"
#include "Platform/IniFile.h"
#include "Platform/Text.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace FasterNGIO::Platform
{
	namespace
	{
		constexpr std::string_view kBaseDirVariable = "%BASE_DIR%";

		[[nodiscard]] int HexDigit(char a_character)
		{
			if (a_character >= '0' && a_character <= '9') {
				return a_character - '0';
			}
			if (a_character >= 'a' && a_character <= 'f') {
				return a_character - 'a' + 10;
			}
			if (a_character >= 'A' && a_character <= 'F') {
				return a_character - 'A' + 10;
			}
			return -1;
		}

		void AppendUtf8(std::string& a_text, std::uint32_t a_codePoint)
		{
			if (a_codePoint < 0x80) {
				a_text.push_back(static_cast<char>(a_codePoint));
			} else if (a_codePoint < 0x800) {
				a_text.push_back(static_cast<char>(0xC0 | (a_codePoint >> 6)));
				a_text.push_back(static_cast<char>(0x80 | (a_codePoint & 0x3F)));
			} else {
				a_text.push_back(static_cast<char>(0xE0 | (a_codePoint >> 12)));
				a_text.push_back(static_cast<char>(0x80 | ((a_codePoint >> 6) & 0x3F)));
				a_text.push_back(static_cast<char>(0x80 | (a_codePoint & 0x3F)));
			}
		}

		[[nodiscard]] std::optional<std::string> ReadBytes(const std::filesystem::path& a_path)
		{
			std::ifstream file(a_path, std::ios::binary);
			if (!file) {
				return std::nullopt;
			}
			return std::string{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
		}

		// A path setting with %BASE_DIR% expanded; a_default when the key is absent or empty.
		[[nodiscard]] std::filesystem::path PathSetting(const IniFile& a_ini, std::string_view a_key, const std::filesystem::path& a_baseDirectory,
			std::string_view a_default)
		{
			auto value = ParseQtIniValue(a_ini.Get("Settings", a_key).value_or(""));
			if (value.empty()) {
				value = a_default;
			}
			// As MO2 expands it: a plain text substitution.
			if (const auto at = value.find(kBaseDirVariable); at != std::string::npos) {
				value.replace(at, kBaseDirVariable.size(), Utf8(a_baseDirectory));
			}
			return PathFromUtf8(value).lexically_normal();
		}

#if defined(_WIN32)
		[[nodiscard]] std::optional<std::wstring> CurrentInstanceName()
		{
			// QSettings' native store for MO2's GlobalSettings ("Mod Organizer Team", "Mod Organizer").
			constexpr auto kKey = L"Software\\Mod Organizer Team\\Mod Organizer";
			DWORD bytes = 0;
			if (RegGetValueW(HKEY_CURRENT_USER, kKey, L"CurrentInstance", RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS) {
				return std::nullopt;
			}
			std::wstring value(bytes / sizeof(wchar_t), L'\0');
			if (RegGetValueW(HKEY_CURRENT_USER, kKey, L"CurrentInstance", RRF_RT_REG_SZ, nullptr, value.data(), &bytes) != ERROR_SUCCESS) {
				return std::nullopt;
			}
			value.resize(bytes / sizeof(wchar_t));
			while (!value.empty() && value.back() == L'\0') {
				value.pop_back();
			}
			return value;
		}
#endif

		struct Candidate
		{
			std::filesystem::path directory;
			std::string name;
		};
	}

	bool Mo2ProfileUsesLocalIni(const std::filesystem::path& a_profile)
	{
		const auto settings = FindInDirectory(a_profile, "settings.ini");
		if (!settings) {
			return false;
		}
		const auto ini = IniFile::Load(*settings);
		const auto value = ini ? LowerAscii(ini->Get("General", "LocalSettings").value_or("")) : std::string{};
		return value == "true" || value == "1";
	}

	std::string Mo2Instance::Describe() const
	{
		return std::format("instance '{}', profile '{}'", instanceName.empty() ? std::string("portable") : instanceName, profileName);
	}

	std::string ParseQtIniValue(std::string_view a_value)
	{
		auto value = Trim(a_value);
		bool byteArray = false;
		if (value.starts_with("@ByteArray(") && value.ends_with(')')) {
			value = value.substr(11, value.size() - 12);
			byteArray = true;
		}
		if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
			value = value.substr(1, value.size() - 2);
		}
		std::string result;
		result.reserve(value.size());
		for (std::size_t i = 0; i < value.size(); ++i) {
			if (value[i] != '\\' || i + 1 == value.size()) {
				result.push_back(value[i]);
				continue;
			}
			switch (const char escaped = value[++i]) {
			case '0':
				result.push_back('\0');
				break;
			case 't':
				result.push_back('\t');
				break;
			case 'n':
				result.push_back('\n');
				break;
			case 'r':
				result.push_back('\r');
				break;
			case 'x':
				{
					std::uint32_t codePoint = 0;
					std::size_t digits = 0;
					while (digits < 4 && i + 1 < value.size() && HexDigit(value[i + 1]) >= 0) {
						codePoint = codePoint * 16 + static_cast<std::uint32_t>(HexDigit(value[++i]));
						++digits;
					}
					// A byte array escapes raw bytes (here, UTF-8); a string escapes code points.
					if (byteArray && codePoint <= 0xFF) {
						result.push_back(static_cast<char>(codePoint));
					} else {
						AppendUtf8(result, codePoint);
					}
					break;
				}
			default:
				// \\, \", \; and anything unknown: the character itself.
				result.push_back(escaped);
				break;
			}
		}
		return result;
	}

	std::expected<Mo2Instance, std::string> ReadMo2Instance(const std::filesystem::path& a_instanceDirectory, std::string a_instanceName)
	{
		const auto iniPath = FindInDirectory(a_instanceDirectory, "ModOrganizer.ini");
		const auto ini = iniPath ? IniFile::Load(*iniPath) : std::nullopt;
		if (!ini) {
			return std::unexpected(std::format("Mod Organizer 2's settings were not found in {}.", Utf8(a_instanceDirectory)));
		}
		const auto gamePath = ParseQtIniValue(ini->Get("General", "gamePath").value_or(""));
		if (gamePath.empty()) {
			return std::unexpected(std::format("{} does not name a game folder.", Utf8(*iniPath)));
		}
		auto game = InspectGameFolder(PathFromUtf8(gamePath));
		if (!game) {
			const auto gameName = ParseQtIniValue(ini->Get("General", "gameName").value_or(""));
			return std::unexpected(std::format("Mod Organizer 2 manages {} at {}: {}", gameName.empty() ? std::string("a game") : gameName, gamePath, game.error()));
		}

		auto baseDirectory = PathSetting(*ini, "base_directory", {}, "");
		if (baseDirectory.empty()) {
			baseDirectory = a_instanceDirectory;
		}
		const auto profiles = PathSetting(*ini, "profiles_directory", baseDirectory, "%BASE_DIR%/profiles");
		const auto profileName = ParseQtIniValue(ini->Get("General", "selected_profile").value_or(""));
		if (profileName.empty()) {
			return std::unexpected(std::format("{} has no selected profile.", Utf8(*iniPath)));
		}
		const auto profile = profiles / PathFromUtf8(profileName);
		if (!IsFile(profile / "plugins.txt")) {
			return std::unexpected(std::format("Mod Organizer 2's profile '{}' has no plugins.txt in {}.", profileName, Utf8(profile)));
		}

		Mo2Instance instance;
		instance.instanceName = std::move(a_instanceName);
		instance.instanceDirectory = a_instanceDirectory;
		instance.profileName = profileName;
		instance.profileDirectory = profile;
		instance.game = std::move(*game);
		instance.game.pluginsTxt = profile / "plugins.txt";
		if (Mo2ProfileUsesLocalIni(profile)) {
			instance.game.iniDirectory = profile;
		}
		return instance;
	}

	std::optional<std::filesystem::path> ModOrganizerDirectory()
	{
#if defined(_WIN32)
		const HMODULE usvfs = GetModuleHandleW(L"usvfs_x64.dll");
		if (!usvfs) {
			return std::nullopt;
		}
		std::wstring buffer(MAX_PATH, L'\0');
		while (true) {
			const auto length = GetModuleFileNameW(usvfs, buffer.data(), static_cast<DWORD>(buffer.size()));
			if (length == 0) {
				return std::nullopt;
			}
			if (length < buffer.size()) {
				buffer.resize(length);
				break;
			}
			buffer.resize(buffer.size() * 2);
		}
		return std::filesystem::path(buffer).parent_path();
#else
		return std::nullopt;
#endif
	}

	std::optional<std::expected<Mo2Instance, std::string>> DetectModOrganizer()
	{
		const auto mo2Directory = ModOrganizerDirectory();
		if (!mo2Directory) {
			return std::nullopt;
		}

		// MO2's current instance first (empty: the portable one beside ModOrganizer.exe), then the
		// rest, in case MO2 was started for another instance (-i).
		std::vector<Candidate> candidates;
		const auto globalRoot = LocalAppDataDirectory().empty() ? std::filesystem::path{} : LocalAppDataDirectory() / "ModOrganizer";
#if defined(_WIN32)
		if (const auto current = CurrentInstanceName(); current && !current->empty()) {
			candidates.push_back({ globalRoot / *current, Utf8(std::filesystem::path(*current)) });
		}
#endif
		candidates.push_back({ *mo2Directory, {} });
		std::error_code error;
		if (IsDirectory(globalRoot)) {
			for (std::filesystem::directory_iterator it(globalRoot, error), end; !error && it != end; it.increment(error)) {
				if (it->is_directory(error)) {
					candidates.push_back({ it->path(), Utf8(it->path().filename()) });
				}
			}
		}

		// The VFS shows the game the selected profile's plugins.txt; the instance whose profile
		// matches it is the one running.
		std::vector<Mo2Instance> valid;
		std::string firstError;
		for (const auto& candidate : candidates) {
			if (std::ranges::any_of(valid, [&](const Mo2Instance& a_instance) { return a_instance.instanceDirectory == candidate.directory; }) ||
				!FindInDirectory(candidate.directory, "ModOrganizer.ini")) {
				continue;
			}
			auto instance = ReadMo2Instance(candidate.directory, candidate.name);
			if (!instance) {
				if (firstError.empty()) {
					firstError = instance.error();
				}
				continue;
			}
			const auto shown = InspectGameFolder(instance->game.root);
			const auto seen = shown ? ReadBytes(shown->pluginsTxt) : std::nullopt;
			if (seen && seen == ReadBytes(instance->game.pluginsTxt)) {
				return std::move(*instance);
			}
			valid.push_back(std::move(*instance));
		}
		if (valid.size() == 1) {
			return std::move(valid.front());
		}
		if (valid.empty()) {
			return std::unexpected(firstError.empty() ? std::format("No Mod Organizer 2 instance was found for {}.", Utf8(*mo2Directory)) : firstError);
		}
		return std::unexpected(std::string("Several Mod Organizer 2 instances manage Skyrim Special Edition, and none matches the load order MO2 shows the game."));
	}
}
