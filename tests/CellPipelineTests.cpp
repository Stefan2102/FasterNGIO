#include "Pipeline/CellPipeline.h"
#include "Pipeline/FileWriterPool.h"

#include "TestSupport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <stop_token>
#include <vector>

namespace
{
	using namespace FasterNGIO;

	// Three LANDs without textures: each cell places nothing and writes an empty cache.
	struct EmptyWorld
	{
		Tests::TempDirectory output;
		GameData::StaticWorldSnapshot snapshot;
		std::vector<GameData::LandInfo> lands;
		std::unordered_map<GameData::FormID, Rejection::QueryShape, GameData::FormIDHash> shapes;

		EmptyWorld()
		{
			for (std::int32_t x = 0; x < 3; ++x) {
				auto& land = lands.emplace_back();
				land.cellX = x;
				land.cellY = -1;
				land.hasHeights = true;
			}
		}

		[[nodiscard]] Pipeline::CellPipelineDesc Desc()
		{
			Pipeline::CellPipelineDesc desc;
			desc.snapshot = &snapshot;
			for (const auto& land : lands) {
				desc.lands.push_back(&land);
			}
			desc.worldEditorID = "Test";
			desc.outputDirectory = output.Path();
			desc.shapesByGrass = &shapes;
			return desc;
		}
	};
}

TEST(CellPipeline, WritesEveryCellInline)
{
	EmptyWorld world;
	std::atomic<std::uint32_t> progress{ 0 };
	auto desc = world.Desc();
	desc.progress = &progress;
	const auto stats = Pipeline::RunCellPipeline(desc);
	EXPECT_EQ(stats.cellsWritten, 3u);
	EXPECT_EQ(stats.cellsFailed, 0u);
	EXPECT_EQ(progress.load(), 3u);
	// An empty cell still gets its 4-byte file: NGIO treats a missing file differently.
	for (const auto* name : { "Testx0000y-001.cgid", "Testx0001y-001.cgid", "Testx0002y-001.cgid" }) {
		EXPECT_EQ(Tests::ReadAll(world.output.Path() / name), (std::vector<std::uint8_t>{ 0, 0, 0, 0 })) << name;
	}

	// Existing caches are kept unless overwriting.
	const auto again = Pipeline::RunCellPipeline(world.Desc());
	EXPECT_EQ(again.cellsSkipped, 3u);
	EXPECT_EQ(again.cellsWritten, 0u);
}

TEST(CellPipeline, HandsFilesToTheWriter)
{
	EmptyWorld world;
	auto tally = std::make_shared<Pipeline::WriteTally>();
	{
		Pipeline::FileWriterPool writer(1, 1ull << 20);
		auto desc = world.Desc();
		desc.writer = &writer;
		desc.writeTally = tally;
		EXPECT_EQ(Pipeline::RunCellPipeline(desc).cellsWritten, 3u);
		writer.Drain();
	}
	EXPECT_EQ(tally->written.load(), 3u);
}

TEST(CellPipeline, CancelsCellsOnceStopped)
{
	EmptyWorld world;
	std::stop_source stop;
	stop.request_stop();
	auto desc = world.Desc();
	desc.stop = stop.get_token();
	const auto stats = Pipeline::RunCellPipeline(desc);
	EXPECT_EQ(stats.cellsCancelled, 3u);
	EXPECT_EQ(stats.cellsWritten, 0u);
	EXPECT_TRUE(std::filesystem::is_empty(world.output.Path()));
}
