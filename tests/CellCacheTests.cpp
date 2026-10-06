#include "Grass/CellCache.h"
#include "Grass/Internal/PlacementCommon.h"

#include <gtest/gtest.h>

#include <bit>

namespace
{
	using namespace FasterNGIO;
	using Grass::Internal::FloatToHalfBits;

	// A blade of a_cell at block-relative (a_x, a_y) and height a_z, its remaining words tagged with a_tag.
	void AddBlade(Grass::CellCandidates& a_cell, float a_x, float a_y, float a_z, std::uint8_t a_quadrant = 0, std::uint16_t a_tag = 0)
	{
		auto& blade = a_cell.blades.emplace_back();
		blade.words.fill(a_tag);
		blade.words[0] = FloatToHalfBits(a_x);
		blade.words[1] = FloatToHalfBits(a_y);
		blade.words[2] = FloatToHalfBits(a_z);
		blade.quadrant = a_quadrant;
	}

	Grass::CellCandidates MakeCell(const GameData::GrassInfo& a_grass, std::int32_t a_cellX = 0, std::int32_t a_cellY = 0)
	{
		Grass::CellCandidates candidates;
		candidates.cellX = a_cellX;
		candidates.cellY = a_cellY;
		candidates.groups.push_back(Grass::CellGrassGroup{ .grass = &a_grass, .modelPath = "meshes\\grass\\test.nif" });
		return candidates;
	}

	[[nodiscard]] float Word(const Grass::NgioGrassGeometryBlock& a_block, std::size_t a_index)
	{
		return std::bit_cast<float>(a_block.descriptorWords[a_index]);
	}
}

TEST(FinalizeCell, RejectionDropsBlades)
{
	GameData::GrassInfo grass{};
	grass.formID = GameData::FormID{ 0x1234 };
	auto candidates = MakeCell(grass);
	for (int i = 0; i < 3; ++i) {
		AddBlade(candidates, static_cast<float>(i * 10), 0.0f, 100.0f, 0, static_cast<std::uint16_t>(i + 1));
	}

	const auto all = Grass::FinalizeCell(candidates, {}).cache;
	ASSERT_EQ(all.groups.size(), 1u);
	ASSERT_EQ(all.groups[0].blocks.size(), 1u);
	EXPECT_EQ(all.groups[0].blocks[0].descriptorWords[7], 3u);

	const std::uint32_t rejectLast = 1u << 2;
	const auto filtered = Grass::FinalizeCell(candidates, std::span(&rejectLast, 1)).cache;
	ASSERT_EQ(filtered.groups.size(), 1u);
	const auto& block = filtered.groups[0].blocks[0];
	EXPECT_EQ(block.descriptorWords[7], 2u);
	EXPECT_EQ(block.descriptorWords[8], Grass::kBladeWords);
	EXPECT_EQ(block.payloadWords.size(), 32u);
	EXPECT_EQ(block.payloadWords[Grass::kBladeWords + 3], 2u);

	const std::uint32_t rejectAll = 0b111;
	EXPECT_TRUE(Grass::FinalizeCell(candidates, std::span(&rejectAll, 1)).cache.groups.empty());
}

TEST(FinalizeCell, DescriptorIsTheEnginesCenterAndExtent)
{
	GameData::GrassInfo grass{};
	// Cell 13 lies in the block starting at cell 12, so x is stored relative to 12 * 4096.
	auto candidates = MakeCell(grass, 13, -1);
	AddBlade(candidates, 4196.0f, 100.0f, 10.0f);
	AddBlade(candidates, 4396.0f, 300.0f, 50.0f);

	const auto cache = Grass::FinalizeCell(candidates, {}).cache;
	const auto& block = cache.groups.at(0).blocks.at(0);
	// Bounds of the stored positions, padded by 30, as center and half extents.
	EXPECT_EQ(Word(block, 0), 49152.0f + 4296.0f);
	EXPECT_EQ(Word(block, 1), 200.0f);
	EXPECT_EQ(Word(block, 2), 30.0f);
	EXPECT_EQ(Word(block, 3), 130.0f);
	EXPECT_EQ(Word(block, 4), 130.0f);
	EXPECT_EQ(Word(block, 5), 50.0f);
	EXPECT_EQ(block.descriptorWords[6], 0u);
	EXPECT_EQ(block.descriptorWords[7], 2u);
}

TEST(FinalizeCell, BlocksSplitPerQuadrantAndModel)
{
	GameData::GrassInfo grass{};
	grass.formID = GameData::FormID{ 0x42 };
	auto candidates = MakeCell(grass);
	// Placement order interleaves the quadrants; blocks keep each quadrant's own order.
	for (std::uint16_t i = 0; i < 13; ++i) {
		AddBlade(candidates, 10.0f, 10.0f, 0.0f, i % 3 == 2 ? 2 : 0, i);
	}
	const std::unordered_map<GameData::FormID, std::uint32_t, GameData::FormIDHash> perBlock{ { grass.formID, 4u } };

	const auto cache = Grass::FinalizeCell(candidates, {}, Grass::BlockLayout{ .bladesPerBlock = &perBlock }).cache;
	const auto& blocks = cache.groups.at(0).blocks;
	ASSERT_EQ(blocks.size(), 4u);
	EXPECT_EQ(blocks[0].descriptorWords[7], 4u);
	EXPECT_EQ(blocks[1].descriptorWords[7], 4u);
	EXPECT_EQ(blocks[2].descriptorWords[7], 1u);
	EXPECT_EQ(blocks[3].descriptorWords[7], 4u);
	// Quadrant 0 holds blades 0, 1, 3, 4, 6, 7, 9, 10, 12; quadrant 2 holds 2, 5, 8, 11.
	EXPECT_EQ(blocks[1].payloadWords[3], 6u);
	EXPECT_EQ(blocks[2].payloadWords[3], 12u);
	EXPECT_EQ(blocks[3].payloadWords[3], 2u);
	EXPECT_EQ(blocks[3].payloadWords[3 * Grass::kBladeWords + 3], 11u);

	// Without a measured model, blocks hold up to the engine's read buffer.
	EXPECT_EQ(Grass::FinalizeCell(candidates, {}).cache.groups.at(0).blocks.size(), 2u);
}

TEST(FinalizeCell, QuadrantCapThinsEvenly)
{
	GameData::GrassInfo grass{};
	auto candidates = MakeCell(grass);
	const auto count = Grass::kMaxBladesPerQuadrant * 2;
	for (std::uint32_t i = 0; i < count; ++i) {
		AddBlade(candidates, 10.0f, 10.0f, 0.0f, 1, static_cast<std::uint16_t>(i));
	}

	const auto capped = Grass::FinalizeCell(candidates, {});
	EXPECT_EQ(capped.bladesCapped, Grass::kMaxBladesPerQuadrant);
	const auto& blocks = capped.cache.groups.at(0).blocks;
	ASSERT_EQ(blocks.size(), 1u);
	ASSERT_EQ(blocks[0].descriptorWords[7], Grass::kMaxBladesPerQuadrant);
	// Every other blade, from the first patch to the last.
	EXPECT_EQ(blocks[0].payloadWords[3], 1u);
	EXPECT_EQ(blocks[0].payloadWords[Grass::kBladeWords + 3], 3u);
	EXPECT_EQ(blocks[0].payloadWords[(Grass::kMaxBladesPerQuadrant - 1) * Grass::kBladeWords + 3], static_cast<std::uint16_t>(count - 1));

	// Uncapped, every blade stays, still in blocks the game can read.
	const auto uncapped = Grass::FinalizeCell(candidates, {}, Grass::BlockLayout{ .capQuadrantBlades = false });
	EXPECT_EQ(uncapped.bladesCapped, 0u);
	const auto& all = uncapped.cache.groups.at(0).blocks;
	ASSERT_EQ(all.size(), 2u);
	EXPECT_EQ(all[0].descriptorWords[7], Grass::kMaxBladesPerBlock);
	EXPECT_EQ(all[1].descriptorWords[7], count - Grass::kMaxBladesPerBlock);
}

TEST(FinalizeCell, BladesPerBlockMatchesTheEngine)
{
	EXPECT_EQ(Grass::kMaxBladesPerBlock, 8192u);
	EXPECT_EQ(Grass::kMaxBladesPerQuadrant, 8191u);
	// min(0xFFFF / (3 * triangles), 0xFFFF / vertices).
	EXPECT_EQ(Grass::BladesPerBlock(20, 30), 1092u);
	EXPECT_EQ(Grass::BladesPerBlock(10, 200), 327u);
	// Tiny models are bounded by the read buffer, broken ones fall back to it.
	EXPECT_EQ(Grass::BladesPerBlock(1, 3), Grass::kMaxBladesPerBlock);
	EXPECT_EQ(Grass::BladesPerBlock(0, 3), Grass::kMaxBladesPerBlock);
	EXPECT_EQ(Grass::BladesPerBlock(30000, 60000), 1u);
}

TEST(FinalizeCell, CacheFileNameMatchesNgio)
{
	EXPECT_EQ(Grass::MakeNgioCacheFileName("Tamriel", -1, 12), "Tamrielx-001y0012.cgid");
	// Grass Cache Helper NG's seasonal name.
	EXPECT_EQ(Grass::MakeNgioCacheFileName("Tamriel", -1, 12, "WIN"), "Tamrielx-001y0012.WIN.cgid");
	EXPECT_EQ(Grass::MakeNgioCacheFileName("Tamriel", -1, 12, ""), "Tamrielx-001y0012.cgid");
}
