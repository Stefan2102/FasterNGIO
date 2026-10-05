#include "Platform/GameInstall.h"
#include "Platform/UserSettings.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
	using namespace FasterNGIO;

	class TempDirectory
	{
	public:
		TempDirectory()
		{
			static std::atomic<int> counter{ 0 };
			_path = std::filesystem::temp_directory_path() / ("fasterngio-install-test-" + std::to_string(counter++) + "-" + std::to_string(std::rand()));
			std::filesystem::create_directories(_path);
		}
		~TempDirectory()
		{
			std::error_code error;
			std::filesystem::remove_all(_path, error);
		}
		[[nodiscard]] const std::filesystem::path& Path() const { return _path; }

	private:
		std::filesystem::path _path;
	};

	void Touch(const std::filesystem::path& a_path)
	{
		std::filesystem::create_directories(a_path.parent_path());
		std::ofstream(a_path, std::ios::binary) << "x";
	}

	// A minimal install: SkyrimSE.exe and Data/Skyrim.esm, plus a_extra in the game folder.
	std::filesystem::path MakeInstall(const std::filesystem::path& a_root, const char* a_extra = nullptr)
	{
		Touch(a_root / "SkyrimSE.exe");
		Touch(a_root / "Data" / "Skyrim.esm");
		if (a_extra) {
			Touch(a_root / a_extra);
		}
		return a_root;
	}

	const Platform::UserFolders kUser{ .localAppData = "/user/local", .documents = "/user/docs" };
}

TEST(GameInstall, AcceptsTheGameFolderAndDerivesTheUserFolders)
{
	const TempDirectory temp;
	const auto root = MakeInstall(temp.Path() / "Skyrim Special Edition");
	const auto install = Platform::InspectGameFolder(root, kUser);
	ASSERT_TRUE(install) << install.error();
	EXPECT_EQ(install->root, root);
	EXPECT_EQ(install->data, root / "Data");
	EXPECT_EQ(install->store, Platform::GameStore::Steam);
	EXPECT_EQ(install->pluginsTxt, kUser.localAppData / "Skyrim Special Edition" / "plugins.txt");
	EXPECT_EQ(install->iniDirectory, kUser.documents / "My Games" / "Skyrim Special Edition");
}

TEST(GameInstall, AcceptsTheDataFolder)
{
	const TempDirectory temp;
	const auto root = MakeInstall(temp.Path() / "Game");
	const auto install = Platform::InspectGameFolder(root / "Data", kUser);
	ASSERT_TRUE(install) << install.error();
	EXPECT_EQ(install->root, root);
	EXPECT_EQ(install->data, root / "Data");
}

TEST(GameInstall, DetectsEachStoresFolders)
{
	struct Case
	{
		const char* marker;
		Platform::GameStore store;
		const char* folder;
	};
	for (const auto& [marker, store, folder] : {
			 Case{ "Galaxy64.dll", Platform::GameStore::Gog, "Skyrim Special Edition GOG" },
			 Case{ "EOSSDK-Win64-Shipping.dll", Platform::GameStore::Epic, "Skyrim Special Edition EPIC" },
			 Case{ "appxmanifest.xml", Platform::GameStore::MicrosoftStore, "Skyrim Special Edition MS" },
		 }) {
		const TempDirectory temp;
		const auto install = Platform::InspectGameFolder(MakeInstall(temp.Path(), marker), kUser);
		ASSERT_TRUE(install) << install.error();
		EXPECT_EQ(install->store, store) << marker;
		EXPECT_EQ(install->pluginsTxt, kUser.localAppData / folder / "plugins.txt") << marker;
		EXPECT_EQ(install->iniDirectory, kUser.documents / "My Games" / folder) << marker;
	}
}

TEST(GameInstall, ExplainsWhatIsMissing)
{
	const TempDirectory temp;
	EXPECT_FALSE(Platform::InspectGameFolder({}, kUser));
	EXPECT_FALSE(Platform::InspectGameFolder(temp.Path() / "nowhere", kUser));

	const auto noExe = Platform::InspectGameFolder(temp.Path(), kUser);
	ASSERT_FALSE(noExe);
	EXPECT_NE(noExe.error().find("SkyrimSE.exe"), std::string::npos);

	Touch(temp.Path() / "SkyrimSE.exe");
	std::filesystem::create_directories(temp.Path() / "Data");
	const auto noEsm = Platform::InspectGameFolder(temp.Path(), kUser);
	ASSERT_FALSE(noEsm);
	EXPECT_NE(noEsm.error().find("Skyrim.esm"), std::string::npos);
}

TEST(GameInstall, MatchesNamesCaseInsensitively)
{
	const TempDirectory temp;
	Touch(temp.Path() / "skyrimse.EXE");
	Touch(temp.Path() / "data" / "SKYRIM.esm");
	const auto install = Platform::InspectGameFolder(temp.Path(), kUser);
	ASSERT_TRUE(install) << install.error();
	EXPECT_EQ(install->data.filename().string().size(), 4u);
}

TEST(GameInstall, MapsASteamLibraryInstallToItsProtonPrefix)
{
	const TempDirectory temp;
	const auto library = temp.Path() / "SteamLibrary" / "steamapps";
	std::filesystem::create_directories(library / "compatdata");
	const auto folders = Platform::ProtonUserFolders(library / "common" / "Skyrim Special Edition");
	ASSERT_TRUE(folders);
	const auto user = library / "compatdata" / "489830" / "pfx" / "drive_c" / "users" / "steamuser";
	EXPECT_EQ(folders->localAppData, user / "AppData" / "Local");
	EXPECT_EQ(folders->documents, user / "Documents");

	EXPECT_FALSE(Platform::ProtonUserFolders(temp.Path() / "Games" / "Skyrim Special Edition"));
}

TEST(UserSettings, RoundTripsValuesAndPaths)
{
	const TempDirectory temp;
	Platform::UserSettings settings;
	settings.Set("placement", "vanilla");
	settings.SetPath("game_folder", std::filesystem::path(u8"D:\\Spiele\\Skyrim Spécial"));
	settings.Set("multi", "one\ntwo");
	const auto path = temp.Path() / "nested" / "settings.ini";
	ASSERT_TRUE(settings.Save(path));

	const auto loaded = Platform::UserSettings::Load(path);
	EXPECT_EQ(loaded.Get("placement"), "vanilla");
	EXPECT_EQ(loaded.GetPath("game_folder"), std::filesystem::path(u8"D:\\Spiele\\Skyrim Spécial"));
	EXPECT_EQ(loaded.Get("multi"), "onetwo");
	EXPECT_FALSE(loaded.Get("missing"));
	EXPECT_FALSE(Platform::UserSettings::Load(temp.Path() / "absent.ini").Get("placement"));
}

TEST(UserSettings, ParsesLinesLeniently)
{
	const auto settings = Platform::UserSettings::Parse("; comment\r\n  key = value with spaces \r\n\r\nnoequals\r\nkey=second\r\n");
	EXPECT_EQ(settings.Get("key"), "value with spaces");
}
