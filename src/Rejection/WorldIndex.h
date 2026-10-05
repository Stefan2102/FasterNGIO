#pragma once

#include "Collision/CollisionModel.h"
#include "GameData/FormID.h"
#include "Rejection/RejectionFeatures.h"

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace FasterNGIO::Archives
{
	class ArchiveResolver;
}

namespace FasterNGIO::GameData
{
	struct StaticWorldSnapshot;
}

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

		// The linear part alone, for directions.
		[[nodiscard]] Float3 ApplyLinear(const Float3& a_direction) const
		{
			return {
				m[0][0] * a_direction.x + m[0][1] * a_direction.y + m[0][2] * a_direction.z,
				m[1][0] * a_direction.x + m[1][1] * a_direction.y + m[1][2] * a_direction.z,
				m[2][0] * a_direction.x + m[2][1] * a_direction.y + m[2][2] * a_direction.z,
			};
		}

		[[nodiscard]] Similarity Inverse() const;

		// A reference's placement: Skyrim's Euler angles (radians) and uniform XSCL scale.
		[[nodiscard]] static Similarity FromPlacement(const float (&a_position)[3], const float (&a_rotation)[3], float a_scale);
	};

	struct ModelRecord
	{
		std::string path;
		Collision::CollisionModel collision{};
	};

	// What an instance is to NGIO's settings, as bits (they are also the GPU's instance masks): an
	// ordinary rejecting object, a grass cliff (grass touching it may move onto it), or an object
	// whose named render shapes do not reject.
	enum InstanceRole : std::uint8_t
	{
		kRoleOrdinary = 1,
		kRoleCliff = 2,
		kRolePartIgnored = 4
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
		InstanceRole role{ kRoleOrdinary };
		// A [CliffObjects] "Steep" cliff: its neighbour check allows a larger height difference.
		bool steep{ false };
	};

	// Where a segment meets an instance's collision surface: t in [0, 1] along it, and the world-space
	// unit normal there, facing against the segment.
	struct WorldSegmentHit
	{
		std::uint32_t instance{ 0 };
		float t{ 0.0f };
		Float3 normal;
	};

	struct WorldIndexStats
	{
		std::uint64_t references{ 0 };
		std::uint64_t referencesWithCollision{ 0 };
		// References of Ray-cast-ignore-forms base forms, left out.
		std::uint64_t referencesIgnored{ 0 };
		// Instances that are grass cliffs, or have ignored render shapes (NGIO).
		std::uint64_t cliffInstances{ 0 };
		std::uint64_t partIgnoredInstances{ 0 };
		std::uint64_t models{ 0 };
		std::uint64_t modelsWithCollision{ 0 };
		// Of those, the models represented by their render geometry (RejectionFeatures::renderGeometry).
		std::uint64_t modelsWithRenderGeometry{ 0 };
		std::uint64_t modelsMissing{ 0 };
		double extractSeconds{ 0.0 };
	};

	// A model placed in the world, for building an index from models already extracted.
	struct PlacedModel
	{
		std::uint32_t model{ 0 };
		Similarity worldFromModel;
		GameData::FormID referenceFormID{};
		GameData::FormID baseFormID{};
		InstanceRole role{ kRoleOrdinary };
		bool steep{ false };
	};

	// Every rejecting instance of one worldspace, binned by the exterior cells its bounds (grown
	// by the widest query reach) overlap.
	class WorldIndex
	{
	public:
		// The worldspace's placed references, with each model's collision extracted from the archives:
		// only collision in a_features' layers, and no references of its ignored base forms.
		WorldIndex(
			const GameData::StaticWorldSnapshot& a_snapshot,
			GameData::FormID a_worldFormID,
			const Archives::ArchiveResolver& a_resolver,
			const RejectionFeatures& a_features,
			float a_maxQueryReach);

		// Models whose collision is already known, placed as given (tests build worlds this way).
		WorldIndex(std::vector<ModelRecord> a_models, std::span<const PlacedModel> a_placements, float a_maxQueryReach);

		[[nodiscard]] std::span<const std::uint32_t> InstancesInCell(std::int32_t a_cellX, std::int32_t a_cellY) const;
		[[nodiscard]] const std::vector<ModelRecord>& Models() const { return _models; }
		[[nodiscard]] const std::vector<Instance>& Instances() const { return _instances; }
		[[nodiscard]] const WorldIndexStats& Stats() const { return _stats; }

	private:
		// Instances a placed model when it has rejecting collision, and bins it.
		void AddInstance(const PlacedModel& a_placement, float a_maxQueryReach);

		std::vector<ModelRecord> _models;
		std::vector<Instance> _instances;
		std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> _cells;
		WorldIndexStats _stats;
	};
}
