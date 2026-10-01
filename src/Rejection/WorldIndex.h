#pragma once

#include "Archives/ArchiveResolver.h"
#include "Collision/CollisionModel.h"
#include "GameData/GameData.h"
#include "Rejection/RejectionConfig.h"

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace FasterNGIO::Rejection
{
	using Collision::Float3;

	// Column-vector similarity transform: p' = linear * p + translation, linear = rotation * scale.
	struct Similarity
	{
		float m[3][4]{ { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };
		float scale{ 1.0f };

		[[nodiscard]] Float3 Apply(const Float3& a_point) const
		{
			return {
				m[0][0] * a_point.x + m[0][1] * a_point.y + m[0][2] * a_point.z + m[0][3],
				m[1][0] * a_point.x + m[1][1] * a_point.y + m[1][2] * a_point.z + m[1][3],
				m[2][0] * a_point.x + m[2][1] * a_point.y + m[2][2] * a_point.z + m[2][3],
			};
		}

		[[nodiscard]] Similarity Inverse() const;

		// A reference's placement: Skyrim's Euler angles (radians) and uniform XSCL scale.
		[[nodiscard]] static Similarity FromPlacement(const float (&a_position)[3], const float (&a_rotation)[3], float a_scale);
	};

	struct ModelRecord
	{
		std::string path;
		Collision::CollisionModel collision;
	};

	// One placed reference whose model has grass-rejecting collision.
	struct Instance
	{
		std::uint32_t model{ 0 };
		GameData::FormID referenceFormID{};
		GameData::FormID baseFormID{};
		Similarity worldFromModel;
		Similarity modelFromWorld;
		Float3 aabbMin;
		Float3 aabbMax;
	};

	struct WorldIndexStats
	{
		std::uint64_t references{ 0 };
		std::uint64_t referencesWithCollision{ 0 };
		std::uint64_t referencesIgnored{ 0 };
		std::uint64_t models{ 0 };
		std::uint64_t modelsWithCollision{ 0 };
		std::uint64_t modelsMissing{ 0 };
		double extractSeconds{ 0.0 };
	};

	// Every rejecting instance of one worldspace, binned by the exterior cells its bounds (grown
	// by the widest query reach) overlap.
	class WorldIndex
	{
	public:
		WorldIndex(
			const GameData::StaticWorldSnapshot& a_snapshot,
			GameData::FormID a_worldFormID,
			const Archives::ArchiveResolver& a_resolver,
			const RejectionConfig& a_config,
			float a_maxQueryReach);

		[[nodiscard]] std::span<const std::uint32_t> InstancesInCell(std::int32_t a_cellX, std::int32_t a_cellY) const;
		[[nodiscard]] const std::vector<ModelRecord>& Models() const { return _models; }
		[[nodiscard]] const std::vector<Instance>& Instances() const { return _instances; }
		[[nodiscard]] const WorldIndexStats& Stats() const { return _stats; }

	private:
		std::vector<ModelRecord> _models;
		std::vector<Instance> _instances;
		std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> _cells;
		WorldIndexStats _stats;
	};

	[[nodiscard]] inline std::uint64_t CellKey(std::int32_t a_x, std::int32_t a_y)
	{
		return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(a_x)) << 32) | static_cast<std::uint32_t>(a_y);
	}
}
