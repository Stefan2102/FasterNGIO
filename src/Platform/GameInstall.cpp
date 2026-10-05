#include "Platform/GameInstall.h"

#include "Platform/DataDirectory.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <system_error>

#if defined(_WIN32)
#include <Windows.h>
#include <ShlObj.h>
#endif

namespace FasterNGIO::Platform
{
	namespace
	{
		constexpr std::string_view kSteamAppId = "489830";

		[[nodiscard]] bool IsDirectory(const std::filesystem::path& a_path)
		{
			std::error_code error;
			return !a_path.empty() && std::filesystem::is_directory(a_path, error);
		}

		[[nodiscard]] bool IEquals(std::string_view a_lhs, std::string_view a_rhs)
		{
			return std::ranges::equal(a_lhs, a_rhs, [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
		}

		[[nodiscard]] std::string Utf8(const std::filesystem::path& a_path)
		{
			const auto text = a_path.u8string();
			return std::string(reinterpret_cast<const char*>(text.data()), text.size());
		}

		[[nodiscard]] UserFolders PrefixUserFolders(const std::filesystem::path& a_prefix)
		{
			const auto user = a_prefix / "drive_c" / "users" / "steamuser";
			return UserFolders{ .localAppData = user / "AppData" / "Local", .documents = user / "Documents" };
		}

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
#endif

		[[nodiscard]] GameStore DetectStore(const std::filesystem::path& a_root)
		{
			if (FindInDirectory(a_root, "Galaxy64.dll")) {
				return GameStore::Gog;
			}
			if (FindInDirectory(a_root, "EOSSDK-Win64-Shipping.dll")) {
				return GameStore::Epic;
			}
			if (FindInDirectory(a_root, "appxmanifest.xml") || FindInDirectory(a_root, "MicrosoftGame.config")) {
				return GameStore::MicrosoftStore;
			}
			return GameStore::Steam;
		}
	}

	std::string_view GameStoreName(GameStore a_store)
	{
		switch (a_store) {
		case GameStore::Gog:
			return "GOG";
		case GameStore::Epic:
			return "Epic Games Store";
		case GameStore::MicrosoftStore:
			return "Microsoft Store";
		case GameStore::Steam:
		default:
			return "Steam";
		}
	}

	std::string GameUserFolderName(GameStore a_store)
	{
		switch (a_store) {
		case GameStore::Gog:
			return "Skyrim Special Edition GOG";
		case GameStore::Epic:
			return "Skyrim Special Edition EPIC";
		case GameStore::MicrosoftStore:
			return "Skyrim Special Edition MS";
		case GameStore::Steam:
		default:
			return "Skyrim Special Edition";
		}
	}

	UserFolders DefaultUserFolders()
	{
#if defined(_WIN32)
		return UserFolders{ .localAppData = KnownFolder(FOLDERID_LocalAppData), .documents = KnownFolder(FOLDERID_Documents) };
#else
		const char* home = std::getenv("HOME");
		if (!home || !*home) {
			return {};
		}
		return PrefixUserFolders(std::filesystem::path(home) / ".steam" / "steam" / "steamapps" / "compatdata" / std::string(kSteamAppId) / "pfx");
#endif
	}

	std::optional<UserFolders> ProtonUserFolders(const std::filesystem::path& a_gameRoot)
	{
		// <library>/steamapps/common/<game>
		const auto common = a_gameRoot.lexically_normal().parent_path();
		const auto steamapps = common.parent_path();
		if (!IEquals(common.filename().string(), "common") || !IEquals(steamapps.filename().string(), "steamapps")) {
			return std::nullopt;
		}
		const auto compatdata = FindInDirectory(steamapps, "compatdata");
		if (!compatdata) {
			return std::nullopt;
		}
		return PrefixUserFolders(*compatdata / std::string(kSteamAppId) / "pfx");
	}

	std::expected<GameInstall, std::string> InspectGameFolder(const std::filesystem::path& a_folder, const std::optional<UserFolders>& a_userFolders)
	{
		if (a_folder.empty()) {
			return std::unexpected(std::string("Choose the folder Skyrim Special Edition is installed in (the one with SkyrimSE.exe)."));
		}
		if (!IsDirectory(a_folder)) {
			return std::unexpected(std::format("{} is not a folder.", Utf8(a_folder)));
		}

		// The Data folder itself is accepted too.
		auto root = a_folder.lexically_normal();
		if (!root.has_filename()) {
			root = root.parent_path();
		}
		if (!FindInDirectory(root, "SkyrimSE.exe") && IEquals(root.filename().string(), "Data") && FindInDirectory(root.parent_path(), "SkyrimSE.exe")) {
			root = root.parent_path();
		}
		if (!FindInDirectory(root, "SkyrimSE.exe")) {
			return std::unexpected(std::format("SkyrimSE.exe is not in {}. Choose the folder Skyrim Special Edition is installed in.", Utf8(root)));
		}
		const auto data = FindInDirectory(root, "Data");
		if (!data || !IsDirectory(*data)) {
			return std::unexpected(std::format("{} has no Data folder.", Utf8(root)));
		}
		if (!FindInDirectory(*data, "Skyrim.esm")) {
			return std::unexpected(std::format("Skyrim.esm is not in {}.", Utf8(*data)));
		}

		GameInstall install;
		install.root = root;
		install.data = *data;
		install.store = DetectStore(root);
		UserFolders folders;
		if (a_userFolders) {
			folders = *a_userFolders;
		} else {
#if defined(_WIN32)
			folders = DefaultUserFolders();
#else
			folders = ProtonUserFolders(root).value_or(DefaultUserFolders());
#endif
		}
		const auto folderName = GameUserFolderName(install.store);
		install.pluginsTxt = folders.localAppData / folderName / "plugins.txt";
		install.iniDirectory = folders.documents / "My Games" / folderName;
		return install;
	}
}
