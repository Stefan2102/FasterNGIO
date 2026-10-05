#include "CommandLine.h"

#include "TestSupport.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	using namespace FasterNGIO;

	[[nodiscard]] std::optional<App::GenerateOptions> Parse(std::initializer_list<std::string_view> a_args)
	{
		const std::vector<std::string_view> args(a_args);
		return App::ParseCommandLine(args);
	}

	// A minimal install, so plugins.txt can be derived from --data.
	struct Game
	{
		Tests::TempDirectory temp;
		std::filesystem::path root = temp.Path() / "Skyrim Special Edition";
		std::string data = (root / "Data").string();

		Game() { Tests::MakeGameFolder(root); }
	};
}

TEST(CommandLine, ParsesTheOptions)
{
	const Game game;
	const auto options = Parse({ "--data", game.data, "--out", "out", "--plugins", "p.txt", "--world", "0x16BB4", "--radius", "5", "-3", "4", "--placement",
		"vanilla", "--reject", "cpu", "--ray-height", "120.5", "--writers", "0" });
	ASSERT_TRUE(options.has_value());
	EXPECT_EQ(options->worlds, std::vector{ GameData::FormID{ 0x16BB4 } });
	EXPECT_EQ(options->centerCellX, 5);
	EXPECT_EQ(options->centerCellY, -3);
	EXPECT_EQ(options->radius, 4);
	EXPECT_EQ(options->placement.mode, Grass::PlacementMode::Vanilla);
	EXPECT_EQ(options->rejection, App::RejectChoice::Cpu);
	EXPECT_FLOAT_EQ(options->rejectionConfig.rayHeight, 120.5f);
	// At least one writer.
	EXPECT_EQ(options->writerThreads, 1u);
	EXPECT_FALSE(options->RunsDiagnostic());
}

TEST(CommandLine, HelpParsesToNothing)
{
	EXPECT_FALSE(Parse({ "--help" }).has_value());
}

TEST(CommandLine, DerivesPluginsTxtFromTheGameFolder)
{
	const Game game;
	Tests::WriteText(game.root / "Galaxy64.dll");
	const auto options = Parse({ "--data", game.data, "--out", "out" });
	ASSERT_TRUE(options.has_value());
	// The GOG build keeps its load order in its own folder.
	EXPECT_EQ(options->pluginsTxtPath.parent_path().filename(), "Skyrim Special Edition GOG");
	EXPECT_EQ(options->pluginsTxtPath.filename(), "plugins.txt");
}

TEST(CommandLine, RejectsInvalidArguments)
{
	const Game game;
	const auto rejects = [&](std::initializer_list<std::string_view> a_args) {
		std::vector<std::string_view> args{ "--data", game.data, "--out", "out" };
		args.insert(args.end(), a_args);
		EXPECT_THROW((void)App::ParseCommandLine(args), std::invalid_argument);
	};
	rejects({ "--threads", "-1" });
	rejects({ "--threads", "010x" });
	rejects({ "--ray-depth", "5cm" });
	rejects({ "--cell", "1", "2", "--radius", "1", "2", "3" });
	rejects({ "--world", "all", "--cell", "1", "2" });
	rejects({ "--world", "0x3C,0x16BB4", "--radius", "1", "2", "3" });
	rejects({ "--world", "0x3C,0x16BB4", "--benchmark-rejection" });
	rejects({ "--world", "0x3C," });
	rejects({ "--collision-survey", "--benchmark-rejection" });
	rejects({ "--validate-cpu", "--reject", "none" });
	rejects({ "--ray-mode", "2" });
	rejects({ "--placement", "pretty" });
	rejects({ "--unknown" });
	rejects({ "--world" });
	// --data and --out are needed outside Mod Organizer 2.
	EXPECT_THROW((void)Parse({ "--out", "out" }), std::invalid_argument);
	EXPECT_THROW((void)Parse({ "--data", game.data }), std::invalid_argument);
	// Diagnostics write no caches, so they need no --out.
	EXPECT_TRUE(Parse({ "--data", game.data, "--collision-survey" }).has_value());
}

TEST(CommandLine, ReadsNumbersAsDecimalUnlessHex)
{
	const Game game;
	const auto options = Parse({ "--data", game.data, "--out", "out", "--threads", "010", "--world", "0X3c" });
	ASSERT_TRUE(options.has_value());
	EXPECT_EQ(options->threads, 10u);
	EXPECT_EQ(options->worlds, std::vector{ GameData::FormID{ 0x3C } });
}

TEST(CommandLine, TakesSeveralWorldspaces)
{
	const Game game;
	const auto options = Parse({ "--data", game.data, "--out", "out", "--world", "0x16BB4,60,0x16bb4" });
	ASSERT_TRUE(options.has_value());
	// In the order given, without repeats.
	EXPECT_EQ(options->worlds, (std::vector{ GameData::FormID{ 0x16BB4 }, GameData::FormID{ 0x3C } }));
	EXPECT_FALSE(options->allWorlds);

	const auto all = Parse({ "--data", game.data, "--out", "out", "--world", "all" });
	ASSERT_TRUE(all.has_value());
	EXPECT_TRUE(all->allWorlds);
}
