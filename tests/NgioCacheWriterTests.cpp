#include "Grass/NgioCacheWriter.h"
#include "Grass/Placement.h"

#include <gtest/gtest.h>

#include <bit>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace
{
	using namespace FasterNGIO;

	std::vector<unsigned char> ReadAll(const std::filesystem::path& a_path)
	{
		std::ifstream input(a_path, std::ios::binary);
		return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
	}

	std::uint32_t U32At(const std::vector<unsigned char>& a_bytes, std::size_t a_offset)
	{
		return static_cast<std::uint32_t>(a_bytes[a_offset]) | (static_cast<std::uint32_t>(a_bytes[a_offset + 1]) << 8) |
		       (static_cast<std::uint32_t>(a_bytes[a_offset + 2]) << 16) | (static_cast<std::uint32_t>(a_bytes[a_offset + 3]) << 24);
	}

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

TEST(NgioCacheWriter, EmptyCellIsOneZeroWord)
{
	const auto path = std::filesystem::temp_directory_path() / "fasterngio_empty.cgid";
	Grass::WriteNgioCellCache(path, {});
	const auto bytes = ReadAll(path);
	ASSERT_EQ(bytes.size(), 4u);
	EXPECT_EQ(U32At(bytes, 0), 0u);
	std::filesystem::remove(path);
}

TEST(FinalizeCell, RejectionDropsBladesAndShrinksBounds)
{
	GameData::GrassInfo grass{};
	grass.formID = GameData::FormID{ 0x1234 };
	grass.heightRange = 8.0f;
	const auto candidates = MakeCandidates(grass);

	const auto all = Grass::FinalizeCell(candidates, {}, 16);
	ASSERT_EQ(all.groups.size(), 1u);
	EXPECT_EQ(all.groups[0].blocks[0].descriptorWords[7], 3u);
	EXPECT_EQ(std::bit_cast<float>(all.groups[0].blocks[0].descriptorWords[3]), 20.0f);

	const std::uint32_t rejectLast = 1u << 2;
	const auto filtered = Grass::FinalizeCell(candidates, std::span(&rejectLast, 1), 16);
	ASSERT_EQ(filtered.groups.size(), 1u);
	const auto& block = filtered.groups[0].blocks[0];
	EXPECT_EQ(block.descriptorWords[7], 2u);
	EXPECT_EQ(block.payloadWords.size(), 32u);
	EXPECT_EQ(std::bit_cast<float>(block.descriptorWords[3]), 10.0f);
	EXPECT_EQ(std::bit_cast<float>(block.descriptorWords[5]), 101.0f + 8.0f);

	const std::uint32_t rejectAll = 0b111;
	EXPECT_TRUE(Grass::FinalizeCell(candidates, std::span(&rejectAll, 1), 16).groups.empty());
}

TEST(Placement, CacheFileNameMatchesNgio)
{
	EXPECT_EQ(Grass::MakeNgioCacheFileName("Tamriel", -1, 12), "Tamrielx-001y0012.cgid");
}
