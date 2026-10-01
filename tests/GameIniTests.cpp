#include "Grass/GameIni.h"
#include "Platform/IniFile.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>

namespace
{
	using namespace FasterNGIO;

	class TempDirectory
	{
	public:
		TempDirectory()
		{
			static std::atomic<int> counter{ 0 };
			_path = std::filesystem::temp_directory_path() / ("fasterngio-ini-test-" + std::to_string(counter++) + "-" + std::to_string(std::rand()));
			std::filesystem::create_directories(_path);
		}
		~TempDirectory()
		{
			std::error_code error;
			std::filesystem::remove_all(_path, error);
		}
		[[nodiscard]] const std::filesystem::path& Path() const { return _path; }
		void Write(const std::string& a_name, const std::string& a_text) const { std::ofstream(_path / a_name, std::ios::binary) << a_text; }

	private:
		std::filesystem::path _path;
	};
}

TEST(IniFile, MatchesNamesCaseInsensitivelyAndKeepsTheFirstValue)
{
	const auto ini = Platform::IniFile::Parse(
		"; comment\r\n"
		"[Grass]\r\n"
		"iMinGrassSize = 40\r\n"
		"# another comment\r\n"
		"IMINGRASSSIZE=10\r\n"
		"[Display]\r\n"
		"iMinGrassSize=99\r\n");
	EXPECT_EQ(ini.Get("grass", "iMinGrassSize").value(), "40");
	EXPECT_EQ(ini.Get("Display", "iMINgrassSize").value(), "99");
	EXPECT_FALSE(ini.Get("Grass", "fTexturePctThreshold").has_value());
}

TEST(GameIni, ReadsSkyrimIniThenSkyrimCustomIniButNotPrefs)
{
	TempDirectory directory;
	directory.Write("skyrim.ini", "[Grass]\niMinGrassSize=40 ; denser grass\niMaxGrassTypesPerTexure=3\nfTexturePctThreshold=0.1\n");
	directory.Write("SkyrimCustom.ini", "[Grass]\nfTexturePctThreshold=0.25\n");
	directory.Write("SkyrimPrefs.ini", "[Grass]\niMinGrassSize=5\n");
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

TEST(GameIni, LeavesUnsetKeysAtEngineDefaults)
{
	TempDirectory directory;
	directory.Write("Skyrim.ini", "[Grass]\nbAllowCreateGrass=1\n");
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
	TempDirectory profile;
	profile.Write("plugins.txt", "*Skyrim.esm\n");
	profile.Write("skyrim.ini", "[Grass]\niMinGrassSize=30\n");
	profile.Write("settings.ini", "[General]\nLocalSettings=true\n");
	const auto located = Grass::LocateGameIniDirectory(profile.Path() / "plugins.txt", std::nullopt);
	ASSERT_TRUE(located.has_value());
	EXPECT_EQ(located->origin, "MO2 profile");
	EXPECT_EQ(located->path, profile.Path());
}

TEST(GameIni, IgnoresAnMo2ProfileWithoutLocalSettings)
{
	TempDirectory profile;
	profile.Write("plugins.txt", "*Skyrim.esm\n");
	profile.Write("settings.ini", "[General]\nLocalSettings=false\n");
	const auto located = Grass::LocateGameIniDirectory(profile.Path() / "plugins.txt", std::nullopt);
	if (located) {
		EXPECT_NE(located->origin, "MO2 profile");
	}
}

TEST(GameIni, PrefersAnExplicitDirectory)
{
	TempDirectory profile;
	TempDirectory explicitDirectory;
	profile.Write("settings.ini", "[General]\nLocalSettings=true\n");
	const auto located = Grass::LocateGameIniDirectory(profile.Path() / "plugins.txt", explicitDirectory.Path());
	ASSERT_TRUE(located.has_value());
	EXPECT_EQ(located->origin, "--game-ini-dir");
	EXPECT_EQ(located->path, explicitDirectory.Path());
}
