#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace FasterNGIO::Grass
{
	struct NgioGrassGeometryBlock
	{
		// Skyrim writes these nine 32-bit descriptor words, then reads
		// bladeCount * vertexStrideWords * sizeof(uint16_t) payload bytes.
		std::array<std::uint32_t, 9> descriptorWords{};
		std::vector<std::uint16_t> payloadWords;
	};

	struct NgioGrassGroup
	{
		std::string modelPath;
		std::uint32_t grassFormID{ 0 };
		bool vertexLighting{ false };
		bool uniformScaling{ false };
		bool fitToSlope{ false };
		std::vector<NgioGrassGeometryBlock> blocks;
	};

	struct NgioCellCache
	{
		std::vector<NgioGrassGroup> groups;
	};

	// The .cgid bytes, as the game reads them (little-endian).
	[[nodiscard]] std::vector<std::uint8_t> SerializeNgioCellCache(const NgioCellCache& a_cache);
}
