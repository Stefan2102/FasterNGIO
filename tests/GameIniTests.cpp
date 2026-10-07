#include "Grass/GameIni.h"
#include "Grass/Placement.h"
#include "Platform/Text.h"

#include "TestSupport.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace
{
	using namespace FasterNGIO;
	using Tests::TempDirectory;
	using Tests::WriteText;

	// A temp game install plus the per-user folders it would use, so lookups never touch the machine's.
	struct FakeSetup
	{
		TempDirectory temp;
		std::filesystem::path game = temp.Path() / "Game";
		Platform::UserFolders folders{ .localAppData = temp.Path() / "Local", .documents = temp.Path() / "Documents" };

		FakeSetup() { Tests::MakeGameFolder(game); }
		[[nodiscard]] std::filesystem::path MyGames(Platform::GameStore a_store) const { return folders.documents / "My Games" / Platform::GameUserFolderName(a_store); }
	};
}

TEST(GameIni, ReadsSkyrimIniThenSkyrimCustomIniButNotPrefs)
{
	TempDirectory directory;
	WriteText(directory.Path() / "skyrim.ini", "[Grass]\niMinGrassSize=40 ; denser grass\niMaxGrassTypesPerTexure=3\nfTexturePctThreshold=0.1\n");
	WriteText(directory.Path() / "SkyrimCustom.ini", "[Grass]\nfTexturePctThreshold=0.25\n");
	WriteText(directory.Path() / "SkyrimPrefs.ini", "[Grass]\niMinGrassSize=5\n");
	const auto ini = Grass::ReadGrassIniSettings(directory.Path());
	ASSERT_TRUE(ini.minGrassSize && ini.maxGrassTypesPerTexture && ini.texturePctThreshold);
	EXPECT_EQ(ini.minGrassSize->value, 40u);
	EXPECT_EQ(ini.maxGrassTypesPerTexture->value, 3u);
	EXPECT_FLOAT_EQ(ini.texturePctThreshold->value, 0.25f);
	EXPECT_EQ(ini.texturePctThreshold->source.filename(), "SkyrimCustom.ini");
	EXPECT_EQ(ini.filesRead.size(), 2u);

	Grass::PlacementSettings settings;
	Grass::ApplyGrassIniSettings(ini, settings);
	EXPECT_EQ(settings.minGrassSize, 40u);
	EXPECT_EQ(settings.maxGrassTypesPerTexture, 3u);
	EXPECT_FLOAT_EQ(settings.alphaThreshold, 0.25f);
}

TEST(GameIni, ReadsTheArchiveLists)
{
	TempDirectory directory;
	WriteText(directory.Path() / "Skyrim.ini", "[Archive]\nsResourceArchiveList2=Skyrim - Patch.bsa,  Custom Grass.bsa ,,Other.bsa\n");
	WriteText(directory.Path() / "SkyrimCustom.ini", "[Archive]\nsResourceArchiveList=Skyrim - Meshes0.bsa\n");
	const auto lists = Grass::ReadArchiveIniLists(directory.Path());
	ASSERT_TRUE(lists.resourceArchiveList && lists.resourceArchiveList2);
	EXPECT_EQ(*lists.resourceArchiveList, (std::vector<std::string>{ "Skyrim - Meshes0.bsa" }));
	EXPECT_EQ(*lists.resourceArchiveList2, (std::vector<std::string>{ "Skyrim - Patch.bsa", "Custom Grass.bsa", "Other.bsa" }));

	TempDirectory empty;
	WriteText(empty.Path() / "Skyrim.ini", "[Grass]\niMinGrassSize=40\n");
	const auto none = Grass::ReadArchiveIniLists(empty.Path());
	EXPECT_FALSE(none.resourceArchiveList || none.resourceArchiveList2);
}

TEST(GameIni, ReadsPluginInisAfterTheGamesInPluginsTxtOrder)
{
	TempDirectory temp;
	const auto data = temp.Path() / "Data";
	const auto inis = temp.Path() / "INI";
	WriteText(inis / "Skyrim.ini", "[Grass]\niMinGrassSize=40\n[Archive]\nsResourceArchiveList2=Base.bsa\n");
	WriteText(inis / "SkyrimCustom.ini", "[Grass]\niMaxGrassTypesPerTexure=4\n");
	// Named for the plugin up to its extension, found case-insensitively; a disabled plugin's and an
	// implicitly loaded master's INIs are not read.
	WriteText(data / "denser grass.INI", "[Grass]\niMinGrassSize=60\nfTexturePctThreshold=0.3\n");
	WriteText(data / "Later.ini", "[Grass]\niMinGrassSize=70\n[Archive]\nsResourceArchiveList2=Base.bsa, Grass.bsa\n");
	WriteText(data / "Disabled.ini", "[Grass]\niMinGrassSize=1\n");
	WriteText(data / "Skyrim.ini", "[Grass]\niMaxGrassTypesPerTexure=1\n");
	const auto pluginsTxt = temp.Path() / "plugins.txt";
	WriteText(pluginsTxt, "# comment\r\n*Denser Grass.esp\r\nDisabled.esp\r\n*NoIni.esm\r\n*Later.ESL\r\n");

	const auto pluginInis = Grass::PluginIniFiles(data, pluginsTxt);
	ASSERT_EQ(pluginInis.size(), 2u);
	EXPECT_EQ(Platform::LowerAscii(pluginInis[0].filename().string()), "denser grass.ini");
	EXPECT_EQ(Platform::LowerAscii(pluginInis[1].filename().string()), "later.ini");

	const auto files = Grass::SkyrimIniFiles(inis, pluginInis);
	ASSERT_EQ(files.size(), 4u);
	const auto ini = Grass::ReadGrassIniSettings(files);
	ASSERT_TRUE(ini.minGrassSize && ini.maxGrassTypesPerTexture && ini.texturePctThreshold);
	EXPECT_EQ(ini.minGrassSize->value, 70u);
	EXPECT_EQ(ini.minGrassSize->source.filename(), "Later.ini");
	EXPECT_EQ(ini.maxGrassTypesPerTexture->value, 4u);
	EXPECT_FLOAT_EQ(ini.texturePctThreshold->value, 0.3f);
	EXPECT_EQ(ini.filesRead.size(), 4u);

	const auto lists = Grass::ReadArchiveIniLists(files);
	ASSERT_TRUE(lists.resourceArchiveList2);
	EXPECT_EQ(*lists.resourceArchiveList2, (std::vector<std::string>{ "Base.bsa", "Grass.bsa" }));

	// Without a game INI folder the plugin INIs still apply.
	EXPECT_EQ(Grass::ReadGrassIniSettings(Grass::SkyrimIniFiles(std::nullopt, pluginInis)).minGrassSize->value, 70u);
}

TEST(GameIni, LeavesUnsetKeysAtEngineDefaults)
{
	TempDirectory directory;
	WriteText(directory.Path() / "Skyrim.ini", "[Grass]\nbAllowCreateGrass=1\n");
	const auto ini = Grass::ReadGrassIniSettings(directory.Path());
	EXPECT_FALSE(ini.minGrassSize.has_value());
	Grass::PlacementSettings settings;
	Grass::ApplyGrassIniSettings(ini, settings);
	EXPECT_EQ(settings.minGrassSize, 20u);
	EXPECT_EQ(settings.maxGrassTypesPerTexture, 2u);
	EXPECT_FLOAT_EQ(settings.alphaThreshold, 0.0f);
}

TEST(GameIni, UsesAnMo2ProfileWithLocalSettings)
{
	FakeSetup setup;
	const auto profile = setup.temp.Path() / "profile";
	WriteText(profile / "plugins.txt", "*Skyrim.esm\n");
	WriteText(profile / "skyrim.ini", "[Grass]\niMinGrassSize=30\n");
	WriteText(profile / "settings.ini", "[General]\nLocalSettings=true\n");
	const auto located = Grass::LocateGameIniDirectory(profile / "plugins.txt", setup.game / "Data", std::nullopt, setup.folders);
	ASSERT_TRUE(located.has_value());
	EXPECT_EQ(located->origin, "MO2 profile");
	EXPECT_EQ(located->path, profile);
}

TEST(GameIni, IgnoresAnMo2ProfileWithoutLocalSettings)
{
	FakeSetup setup;
	const auto profile = setup.temp.Path() / "profile";
	WriteText(profile / "plugins.txt", "*Skyrim.esm\n");
	WriteText(profile / "settings.ini", "[General]\nLocalSettings=false\n");
	std::filesystem::create_directories(setup.MyGames(Platform::GameStore::Steam));
	const auto located = Grass::LocateGameIniDirectory(profile / "plugins.txt", setup.game / "Data", std::nullopt, setup.folders);
	ASSERT_TRUE(located.has_value());
	EXPECT_EQ(located->origin, "My Games");
	EXPECT_EQ(located->path, setup.MyGames(Platform::GameStore::Steam));
}

TEST(GameIni, UsesTheInstallsOwnStoreFolder)
{
	// A GOG install on a machine that also has Steam's My Games folder reads GOG's.
	FakeSetup setup;
	WriteText(setup.game / "Galaxy64.dll");
	std::filesystem::create_directories(setup.MyGames(Platform::GameStore::Steam));
	std::filesystem::create_directories(setup.MyGames(Platform::GameStore::Gog));
	const auto located = Grass::LocateGameIniDirectory(setup.temp.Path() / "plugins.txt", setup.game / "Data", std::nullopt, setup.folders);
	ASSERT_TRUE(located.has_value());
	EXPECT_EQ(located->path, setup.MyGames(Platform::GameStore::Gog));
}

TEST(GameIni, PrefersAnExplicitDirectory)
{
	FakeSetup setup;
	TempDirectory explicitDirectory;
	WriteText(setup.temp.Path() / "profile" / "settings.ini", "[General]\nLocalSettings=true\n");
	const auto located = Grass::LocateGameIniDirectory(setup.temp.Path() / "profile" / "plugins.txt", setup.game / "Data", explicitDirectory.Path(), setup.folders);
	ASSERT_TRUE(located.has_value());
	EXPECT_EQ(located->origin, "--game-ini-dir");
	EXPECT_EQ(located->path, explicitDirectory.Path());
}
