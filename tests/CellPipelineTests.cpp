#include "Pipeline/CellPipeline.h"
#include "Pipeline/FileWriterPool.h"
#include "Grass/LandTexture.h"
#include "Rejection/CpuBvh.h"

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

TEST(CellPipeline, AppliesNgiosGrassFilters)
{
	using GameData::FormID;
	constexpr FormID kGrass{ 0x100 };
	constexpr FormID kTexture{ 0x200 };
	constexpr FormID kWorld{ 0x3C };
	Tests::TempDirectory output;
	GameData::StaticWorldSnapshot snapshot;
	auto& grass = snapshot.grassesByFormID[kGrass];
	grass.formID = kGrass;
	grass.modelPath = "grass.nif";
	grass.density = 50;
	grass.positionRange = 20.0f;
	auto& texture = snapshot.landTexturesByFormID[kTexture];
	texture.formID = kTexture;
	texture.grassFormIDs = { kGrass };
	auto& land = snapshot.landsByWorldspace[kWorld].emplace_back();
	land.cellX = 0;
	land.cellY = 0;
	land.hasHeights = true;
	for (std::uint8_t q = 0; q < GameData::LandInfo::QuadrantCount; ++q) {
		land.baseTextures[q] = GameData::LandBaseTexture{ .landTextureFormID = kTexture, .quadrant = q };
	}
	std::unordered_map<FormID, Rejection::QueryShape, GameData::FormIDHash> shapes{ { kGrass, Rejection::QueryShape{ .depth = 5.0f, .height = 150.0f, .radius = 10.0f } } };
	// Nothing to collide with: only the filters reject.
	const Rejection::WorldIndex world({}, {}, 0.0f);
	const Rejection::CpuBvh bvh(world);
	const Grass::LandTextureMask mask(snapshot, kWorld, { kTexture });
	const Rejection::FormSet ignored{ kGrass };

	const auto run = [&](const Grass::LandTextureMask* a_mask, const Rejection::FormSet* a_ignored) {
		Pipeline::CellPipelineDesc desc;
		desc.snapshot = &snapshot;
		desc.lands = { &land };
		desc.worldEditorID = "Test";
		desc.outputDirectory = output.Path();
		desc.overwrite = true;
		desc.shapesByGrass = &shapes;
		desc.backend = Pipeline::RejectionBackend::Cpu;
		desc.world = &world;
		desc.cpuBvh = &bvh;
		desc.textureMask = a_mask;
		desc.textureWidth = 5.0f;
		desc.ignoredGrass = a_ignored;
		return Pipeline::RunCellPipeline(desc);
	};
	const auto plain = run(nullptr, nullptr);
	ASSERT_GT(plain.blades, 0u);
	EXPECT_EQ(plain.bladesRejected, 0u);
	// Every blade stands on the listed texture.
	const auto textured = run(&mask, nullptr);
	EXPECT_EQ(textured.bladesRejected, textured.blades);
	// Ignored grass is never rejected, texture or not.
	EXPECT_EQ(run(&mask, &ignored).bladesRejected, 0u);
}
