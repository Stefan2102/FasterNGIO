#include "Grass/CellCache.h"

#include <gtest/gtest.h>

#include <bit>

namespace
{
	using namespace FasterNGIO;

	Grass::CellCandidates MakeCandidates(const GameData::GrassInfo& a_grass)
	{
		Grass::CellCandidates candidates;
		candidates.groups.push_back(Grass::CellGrassGroup{ .grass = &a_grass, .modelPath = "meshes\\grass\\test.nif" });
		for (int i = 0; i < 3; ++i) {
			auto& blade = candidates.blades.emplace_back();
			blade.words.fill(static_cast<std::uint16_t>(i + 1));
			blade.position[0] = static_cast<float>(i * 10);
			blade.position[1] = static_cast<float>(-i * 10);
			blade.position[2] = 100.0f + static_cast<float>(i);
		}
		return candidates;
	}
}

TEST(FinalizeCell, RejectionDropsBladesAndShrinksBounds)
{
	GameData::GrassInfo grass{};
	grass.formID = GameData::FormID{ 0x1234 };
	grass.heightRange = 8.0f;
	const auto candidates = MakeCandidates(grass);

	const auto all = Grass::FinalizeCell(candidates, {});
	ASSERT_EQ(all.groups.size(), 1u);
	EXPECT_EQ(all.groups[0].blocks[0].descriptorWords[7], 3u);
	EXPECT_EQ(std::bit_cast<float>(all.groups[0].blocks[0].descriptorWords[3]), 20.0f);

	const std::uint32_t rejectLast = 1u << 2;
	const auto filtered = Grass::FinalizeCell(candidates, std::span(&rejectLast, 1));
	ASSERT_EQ(filtered.groups.size(), 1u);
	const auto& block = filtered.groups[0].blocks[0];
	EXPECT_EQ(block.descriptorWords[7], 2u);
	EXPECT_EQ(block.payloadWords.size(), 32u);
	EXPECT_EQ(std::bit_cast<float>(block.descriptorWords[3]), 10.0f);
	EXPECT_EQ(std::bit_cast<float>(block.descriptorWords[5]), 101.0f + 8.0f);

	const std::uint32_t rejectAll = 0b111;
	EXPECT_TRUE(Grass::FinalizeCell(candidates, std::span(&rejectAll, 1)).groups.empty());
}

TEST(FinalizeCell, CacheFileNameMatchesNgio)
{
	EXPECT_EQ(Grass::MakeNgioCacheFileName("Tamriel", -1, 12), "Tamrielx-001y0012.cgid");
	// Grass Cache Helper NG's seasonal name.
	EXPECT_EQ(Grass::MakeNgioCacheFileName("Tamriel", -1, 12, "WIN"), "Tamrielx-001y0012.WIN.cgid");
	EXPECT_EQ(Grass::MakeNgioCacheFileName("Tamriel", -1, 12, ""), "Tamrielx-001y0012.cgid");
}
