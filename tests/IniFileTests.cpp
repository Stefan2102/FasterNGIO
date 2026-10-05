#include "Platform/IniFile.h"

#include <gtest/gtest.h>

using namespace FasterNGIO;

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
