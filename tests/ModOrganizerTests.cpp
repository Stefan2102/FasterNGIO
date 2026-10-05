#include "Platform/ModOrganizer.h"

#include "TestSupport.h"

#include <gtest/gtest.h>

#include <format>

namespace
{
	using namespace FasterNGIO;
	using Tests::TempDirectory;
	using Tests::WriteText;

	// A ModOrganizer.ini as MO2 writes it, with backslashes doubled inside @ByteArray().
	std::string Mo2Ini(const std::filesystem::path& a_game, std::string_view a_profile, std::string_view a_extraSettings = "")
	{
		std::string escaped;
		for (const auto c : a_game.string()) {
			escaped += c == '\\' ? std::string("\\\\") : std::string(1, c);
		}
		return std::format("[General]\ngameName=Skyrim Special Edition\ngamePath=@ByteArray({})\nselected_profile=@ByteArray({})\n\n[Settings]\n{}\n", escaped,
			a_profile, a_extraSettings);
	}
}

TEST(ModOrganizer, ParsesQtIniValues)
{
	EXPECT_EQ(Platform::ParseQtIniValue("@ByteArray(C:\\\\Games\\\\Skyrim Special Edition)"), "C:\\Games\\Skyrim Special Edition");
	EXPECT_EQ(Platform::ParseQtIniValue("@ByteArray(Default)"), "Default");
	EXPECT_EQ(Platform::ParseQtIniValue("\"a, b\""), "a, b");
	EXPECT_EQ(Platform::ParseQtIniValue("say \\\"hi\\\""), "say \"hi\"");
	EXPECT_EQ(Platform::ParseQtIniValue("plain"), "plain");
	// Byte arrays escape raw (UTF-8) bytes; strings escape code points.
	EXPECT_EQ(Platform::ParseQtIniValue("@ByteArray(Caf\\xc3\\xa9)"), "Caf\xC3\xA9");
	EXPECT_EQ(Platform::ParseQtIniValue("Caf\\xe9"), "Caf\xC3\xA9");
}

TEST(ModOrganizer, ReadsAPortableInstance)
{
	TempDirectory temp;
	const auto game = temp.Path() / "Skyrim Special Edition";
	Tests::MakeGameFolder(game);
	const auto mo2 = temp.Path() / "MO2";
	WriteText(mo2 / "ModOrganizer.ini", Mo2Ini(game, "Default"));
	WriteText(mo2 / "profiles" / "Default" / "plugins.txt", "*Mod.esp\n");

	const auto instance = Platform::ReadMo2Instance(mo2, {});
	ASSERT_TRUE(instance.has_value()) << instance.error();
	EXPECT_EQ(instance->profileName, "Default");
	EXPECT_EQ(instance->game.root, game);
	EXPECT_EQ(instance->game.pluginsTxt.lexically_normal(), (mo2 / "profiles" / "Default" / "plugins.txt").lexically_normal());
	// Without LocalSettings the game's own INI folder stays.
	EXPECT_NE(instance->game.iniDirectory, instance->profileDirectory);
	EXPECT_EQ(instance->Describe(), "instance 'portable', profile 'Default'");
}

TEST(ModOrganizer, ExpandsBaseDirAndUsesProfileInis)
{
	TempDirectory temp;
	const auto game = temp.Path() / "Game";
	Tests::MakeGameFolder(game);
	const auto instance = temp.Path() / "Instance";
	const auto base = temp.Path() / "Base";
	WriteText(instance / "ModOrganizer.ini",
		Mo2Ini(game, "Survival", std::format("base_directory={}\nprofiles_directory=%BASE_DIR%/my profiles", base.generic_string())));
	const auto profile = base / "my profiles" / "Survival";
	WriteText(profile / "plugins.txt", "*Mod.esp\n");
	WriteText(profile / "settings.ini", "[General]\nLocalSettings=true\n");

	const auto read = Platform::ReadMo2Instance(instance, "Main");
	ASSERT_TRUE(read.has_value()) << read.error();
	EXPECT_EQ(read->profileDirectory.lexically_normal(), profile.lexically_normal());
	EXPECT_EQ(read->game.iniDirectory.lexically_normal(), profile.lexically_normal());
	EXPECT_EQ(read->Describe(), "instance 'Main', profile 'Survival'");
}

TEST(ModOrganizer, ExplainsWhatIsMissing)
{
	TempDirectory temp;
	const auto game = temp.Path() / "Game";
	Tests::MakeGameFolder(game);

	const auto noIni = Platform::ReadMo2Instance(temp.Path() / "Nothing", {});
	ASSERT_FALSE(noIni.has_value());
	EXPECT_NE(noIni.error().find("settings were not found"), std::string::npos);

	const auto noProfile = temp.Path() / "NoProfile";
	WriteText(noProfile / "ModOrganizer.ini", Mo2Ini(game, "Missing"));
	const auto missing = Platform::ReadMo2Instance(noProfile, {});
	ASSERT_FALSE(missing.has_value());
	EXPECT_NE(missing.error().find("'Missing' has no plugins.txt"), std::string::npos);

	const auto badGame = temp.Path() / "BadGame";
	WriteText(badGame / "ModOrganizer.ini", Mo2Ini(temp.Path() / "NotAGame", "Default"));
	WriteText(badGame / "profiles" / "Default" / "plugins.txt");
	const auto invalid = Platform::ReadMo2Instance(badGame, {});
	ASSERT_FALSE(invalid.has_value());
	EXPECT_NE(invalid.error().find("Mod Organizer 2 manages Skyrim Special Edition"), std::string::npos);
}

TEST(ModOrganizer, NotDetectedOutsideModOrganizer)
{
	// The test runner is not started from MO2.
	EXPECT_FALSE(Platform::ModOrganizerDirectory().has_value());
	EXPECT_FALSE(Platform::DetectModOrganizer().has_value());
}
