#pragma once

#include "Collision/CollisionModel.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace FasterNGIO::Rejection
{
	// An axis-aligned box, empty until grown. Laid out as a BLAS AABB (min xyz, max xyz).
	struct Bounds
	{
		float min[3]{ (std::numeric_limits<float>::max)(), (std::numeric_limits<float>::max)(), (std::numeric_limits<float>::max)() };
		float max[3]{ -(std::numeric_limits<float>::max)(), -(std::numeric_limits<float>::max)(), -(std::numeric_limits<float>::max)() };

		// Adds a sphere of radius a_reach around a_point.
		void Grow(const Collision::Float3& a_point, float a_reach = 0.0f)
		{
			const float point[3]{ a_point.x, a_point.y, a_point.z };
			for (int axis = 0; axis < 3; ++axis) {
				min[axis] = (std::min)(min[axis], point[axis] - a_reach);
				max[axis] = (std::max)(max[axis], point[axis] + a_reach);
			}
		}

		void Grow(const Bounds& a_other)
		{
			for (int axis = 0; axis < 3; ++axis) {
				min[axis] = (std::min)(min[axis], a_other.min[axis]);
				max[axis] = (std::max)(max[axis], a_other.max[axis]);
			}
		}

		// Half the surface area, the SAH cost of a box.
		[[nodiscard]] float HalfArea() const
		{
			const float x = max[0] - min[0];
			const float y = max[1] - min[1];
			const float z = max[2] - min[2];
			return x * y + y * z + z * x;
		}

		[[nodiscard]] float Centroid(int a_axis) const { return 0.5f * (min[a_axis] + max[a_axis]); }
	};
	static_assert(sizeof(Bounds) == 24);

	// A collision model's primitive kinds, in BLAS geometry order (the GPU has a hit group for each).
	enum class PrimitiveKind : std::uint32_t
	{
		Triangle = 0,
		Hull = 1,
		Capsule = 2
	};
	inline constexpr std::uint32_t kPrimitiveKindCount = 3;

	[[nodiscard]] inline std::uint32_t PrimitiveCount(const Collision::CollisionModel& a_model, PrimitiveKind a_kind)
	{
		switch (a_kind) {
		case PrimitiveKind::Triangle:
			return static_cast<std::uint32_t>(a_model.triangles.size());
		case PrimitiveKind::Hull:
			return static_cast<std::uint32_t>(a_model.hulls.size());
		case PrimitiveKind::Capsule:
		default:
			return static_cast<std::uint32_t>(a_model.capsules.size());
		}
	}

	// A primitive's bounds in model space, grown by its own radius plus a_inflation.
	[[nodiscard]] inline Bounds PrimitiveBounds(const Collision::CollisionModel& a_model, PrimitiveKind a_kind, std::uint32_t a_index, float a_inflation = 0.0f)
	{
		Bounds bounds;
		switch (a_kind) {
		case PrimitiveKind::Triangle:
			for (const auto& vertex : a_model.triangles[a_index].vertices) {
				bounds.Grow(vertex, a_model.triangles[a_index].radius + a_inflation);
			}
			break;
		case PrimitiveKind::Hull:
			bounds.Grow(a_model.hulls[a_index].aabbMin, a_model.hulls[a_index].radius + a_inflation);
			bounds.Grow(a_model.hulls[a_index].aabbMax, a_model.hulls[a_index].radius + a_inflation);
			break;
		case PrimitiveKind::Capsule:
			bounds.Grow(a_model.capsules[a_index].p0, a_model.capsules[a_index].radius + a_inflation);
			bounds.Grow(a_model.capsules[a_index].p1, a_model.capsules[a_index].radius + a_inflation);
			break;
		}
		return bounds;
	}
}
