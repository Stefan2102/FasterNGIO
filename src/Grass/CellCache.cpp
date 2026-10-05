#include "Grass/CellCache.h"

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <fstream>
#include <limits>
#include <system_error>
#include <vector>

namespace FasterNGIO::Grass
{
	NgioCellCache FinalizeCell(const CellCandidates& a_candidates, std::span<const std::uint32_t> a_rejected)
	{
		struct GroupBuild
		{
			std::vector<std::uint16_t> bladeWords;
			float min[3]{ (std::numeric_limits<float>::max)(), (std::numeric_limits<float>::max)(), (std::numeric_limits<float>::max)() };
			float max[3]{ -(std::numeric_limits<float>::max)(), -(std::numeric_limits<float>::max)(), -(std::numeric_limits<float>::max)() };
		};

		std::vector<GroupBuild> builds(a_candidates.groups.size());
		for (std::size_t i = 0; i < a_candidates.blades.size(); ++i) {
			if (!a_rejected.empty() && (a_rejected[i / 32] & (1u << (i % 32))) != 0) {
				continue;
			}
			const auto& blade = a_candidates.blades[i];
			auto& build = builds[blade.groupIndex];
			const auto& grass = *a_candidates.groups[blade.groupIndex].grass;
			build.bladeWords.insert(build.bladeWords.end(), blade.words.begin(), blade.words.end());
			for (int axis = 0; axis < 2; ++axis) {
				build.min[axis] = (std::min)(build.min[axis], blade.position[axis]);
				build.max[axis] = (std::max)(build.max[axis], blade.position[axis]);
			}
			// Bounds reach the top of the tallest blade (at least a unit above the ground).
			build.min[2] = (std::min)(build.min[2], blade.position[2]);
			build.max[2] = (std::max)(build.max[2], blade.position[2] + (std::max)(grass.heightRange, 1.0f));
		}

		std::vector<std::uint32_t> order;
		for (std::uint32_t i = 0; i < builds.size(); ++i) {
			if (!builds[i].bladeWords.empty()) {
				order.push_back(i);
			}
		}
		std::ranges::sort(order, [&](std::uint32_t lhs, std::uint32_t rhs) {
			return a_candidates.groups[lhs].grass->formID < a_candidates.groups[rhs].grass->formID;
		});

		NgioCellCache cache;
		for (const auto index : order) {
			const auto& source = a_candidates.groups[index];
			auto& build = builds[index];
			NgioGrassGroup group;
			group.modelPath = source.modelPath;
			group.grassFormID = source.grass->formID.value;
			group.vertexLighting = source.grass->HasVertexLighting();
			group.uniformScaling = source.grass->HasUniformScaling();
			group.fitToSlope = source.grass->FitsToSlope();
			NgioGrassGeometryBlock block;
			const auto bladeCount = static_cast<std::uint32_t>(build.bladeWords.size() / kBladeWords);
			block.descriptorWords = {
				std::bit_cast<std::uint32_t>(build.min[0]),
				std::bit_cast<std::uint32_t>(build.min[1]),
				std::bit_cast<std::uint32_t>(build.min[2]),
				std::bit_cast<std::uint32_t>(build.max[0]),
				std::bit_cast<std::uint32_t>(build.max[1]),
				std::bit_cast<std::uint32_t>(build.max[2]),
				0u,
				bladeCount,
				kBladeWords,
			};
			block.payloadWords = std::move(build.bladeWords);
			group.blocks.push_back(std::move(block));
			cache.groups.push_back(std::move(group));
		}
		return cache;
	}

	std::string MakeNgioCacheFileName(std::string_view a_worldEditorID, std::int32_t a_cellX, std::int32_t a_cellY)
	{
		return std::format("{}x{:04}y{:04}.cgid", a_worldEditorID, a_cellX, a_cellY);
	}

	std::string ResolveWorldEditorID(const GameData::StaticWorldSnapshot& a_snapshot, GameData::FormID a_worldFormID)
	{
		const auto worldIt = a_snapshot.worldsByFormID.find(a_worldFormID);
		if (worldIt != a_snapshot.worldsByFormID.end() && !worldIt->second.editorID.empty()) {
			return worldIt->second.editorID;
		}
		if (a_worldFormID.value == 0x0000003Cu) {
			return "Tamriel";
		}
		return std::format("{:08X}", a_worldFormID.value);
	}

	bool ExistingNgioCacheLooksValid(const std::filesystem::path& a_path)
	{
		// More groups than this is not a cache NGIO wrote.
		constexpr std::uint32_t kMaxPlausibleGroups = 4096;
		std::error_code ec;
		const auto size = std::filesystem::file_size(a_path, ec);
		if (ec || size < sizeof(std::uint32_t)) {
			return false;
		}
		std::ifstream input(a_path, std::ios::binary);
		std::array<unsigned char, 4> bytes{};
		input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		if (!input) {
			return false;
		}
		const auto groupCount = static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8u) |
		                        (static_cast<std::uint32_t>(bytes[2]) << 16u) | (static_cast<std::uint32_t>(bytes[3]) << 24u);
		return groupCount <= kMaxPlausibleGroups;
	}
}
