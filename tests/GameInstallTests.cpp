#include "Platform/GameInstall.h"

#include "TestSupport.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace
{
	using namespace FasterNGIO;
	using Tests::TempDirectory;

	void Touch(const std::filesystem::path& a_path) { Tests::WriteText(a_path); }

	// A minimal install, plus a_extra in the game folder.
	std::filesystem::path MakeInstall(const std::filesystem::path& a_root, const char* a_extra = nullptr)
	{
		Tests::MakeGameFolder(a_root);
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
