#include "Platform/Text.h"
#include "Platform/UserSettings.h"

#include "TestSupport.h"

#include <gtest/gtest.h>

#include <filesystem>

using namespace FasterNGIO;

TEST(UserSettings, RoundTripsValuesAndPaths)
{
	const Tests::TempDirectory temp;
	const std::filesystem::path folder(u8"D:\\Spiele\\Skyrim Spécial");
	Platform::UserSettings settings;
	settings.Set("placement", "vanilla");
	settings.Set("game_folder", Platform::Utf8(folder));
	settings.Set("multi", "one\ntwo");
	const auto path = temp.Path() / "nested" / "settings.ini";
	ASSERT_TRUE(settings.Save(path));

	const auto loaded = Platform::UserSettings::Load(path);
	EXPECT_EQ(loaded.Get("placement"), "vanilla");
	EXPECT_EQ(Platform::PathFromUtf8(loaded.Get("game_folder").value_or("")), folder);
	EXPECT_EQ(loaded.Get("multi"), "onetwo");
	EXPECT_FALSE(loaded.Get("missing"));
	EXPECT_FALSE(Platform::UserSettings::Load(temp.Path() / "absent.ini").Get("placement"));
}

TEST(UserSettings, ParsesLinesLeniently)
{
	const auto settings = Platform::UserSettings::Parse("; comment\r\n  key = value with spaces \r\n\r\nnoequals\r\nkey=second\r\n");
	EXPECT_EQ(settings.Get("key"), "value with spaces");
}
