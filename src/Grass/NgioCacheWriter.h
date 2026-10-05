#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace FasterNGIO::Grass
{
	struct NgioGrassGeometryBlock
	{
		static constexpr std::size_t EncodedSize = 0x24;

		// Skyrim writes these nine 32-bit descriptor words, then reads
		// bladeCount * vertexStrideWords * sizeof(uint16_t) payload bytes.
		std::array<std::uint32_t, 9> descriptorWords{};
		std::vector<std::uint16_t> payloadWords;
	};

	struct NgioGrassGroup
	{
		std::string modelPath;
		std::uint32_t grassModelData{ 0 };
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

	// Creates a_path's folder and writes the file in one call.
	void WriteNgioCellCache(const std::filesystem::path& a_path, const NgioCellCache& a_cache);
}
