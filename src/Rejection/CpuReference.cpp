#include "Rejection/CpuReference.h"

#include "Grass/Placement.h"
#include "Rejection/HlslShim.h"
#include "Rejection/PrimitiveTests.h"

#include <cmath>
#include <format>
#include <stdexcept>

namespace FasterNGIO::Rejection
{
	bool CapsuleOverlapsInstance(const WorldIndex& a_world, const Instance& a_instance, const Float3& a_p, const Float3& a_q, float a_radius)
	{
		const auto& model = a_world.Models()[a_instance.model].collision;
		const auto p = ToHlsl(a_instance.modelFromWorld.Apply(a_p));
		const auto q = ToHlsl(a_instance.modelFromWorld.Apply(a_q));
		const auto r = a_radius * a_instance.modelFromWorld.scale;
		if (!SegmentOverlapsAabb(p, q, r, model.aabbMin, model.aabbMax)) {
			return false;
		}
		for (const auto& tri : model.triangles) {
			if (CapsuleOverlapsTriangle(tri, p, q, r)) {
				return true;
			}
		}
		for (const auto& hull : model.hulls) {
			if (CapsuleOverlapsHull(model, hull, p, q, r)) {
				return true;
			}
		}
		for (const auto& capsule : model.capsules) {
			if (CapsuleOverlapsCapsule(capsule, p, q, r)) {
				return true;
			}
		}
		return false;
	}

	std::string ExplainCapsule(const WorldIndex& a_world, std::int32_t a_cellX, std::int32_t a_cellY, const Float3& a_p, const Float3& a_q, float a_radius)
	{
		std::string out;
		for (const auto index : a_world.InstancesInCell(a_cellX, a_cellY)) {
			const auto& instance = a_world.Instances()[index];
			if (!CapsuleOverlapsInstance(a_world, instance, a_p, a_q, a_radius)) {
				continue;
			}
			const auto& model = a_world.Models()[instance.model].collision;
			const auto p = ToHlsl(instance.modelFromWorld.Apply(a_p));
			const auto q = ToHlsl(instance.modelFromWorld.Apply(a_q));
			const auto r = a_radius * instance.modelFromWorld.scale;
			std::string kinds;
			for (std::size_t i = 0; i < model.triangles.size(); ++i) {
				const auto& tri = model.triangles[i];
				if (Hlsl::CapsuleOverlapsTriangle(p, q, r, ToHlsl(tri.vertices[0]), ToHlsl(tri.vertices[1]), ToHlsl(tri.vertices[2]), tri.radius)) {
					const auto distance = std::sqrt(Hlsl::SegmentTriangleDistanceSq(p, q, ToHlsl(tri.vertices[0]), ToHlsl(tri.vertices[1]), ToHlsl(tri.vertices[2])));
					const auto edge = [](const Float3& a, const Float3& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); };
					kinds += std::format(" tri#{} dist={:.3f} reach={:.3f} seg=({:.1f},{:.1f},{:.1f})-({:.1f},{:.1f},{:.1f}) edges={:.0f}/{:.0f}/{:.0f}", i, distance, r + tri.radius,
						p.x, p.y, p.z, q.x, q.y, q.z,
						edge(tri.vertices[0], tri.vertices[1]), edge(tri.vertices[1], tri.vertices[2]), edge(tri.vertices[2], tri.vertices[0]));
					break;
				}
			}
			for (std::size_t i = 0; i < model.hulls.size(); ++i) {
				const auto& hull = model.hulls[i];
				if (HullContains(model, hull, p) || HullContains(model, hull, q)) {
					kinds += std::format(" hull#{}(contains)", i);
				}
				for (std::uint32_t t = 0; t < hull.triangleCount; ++t) {
					const auto& tri = model.hullTriangles[hull.firstTriangle + t];
					const auto distance = std::sqrt(Hlsl::SegmentTriangleDistanceSq(p, q, ToHlsl(tri.vertices[0]), ToHlsl(tri.vertices[1]), ToHlsl(tri.vertices[2])));
					if (distance <= r + hull.radius) {
						const auto& plane = model.hullPlanes[hull.firstPlane];
						kinds += std::format(" hull#{} face{} dist={:.3f} reach={:.3f} planes={} plane0=({:.3f},{:.3f},{:.3f},{:.3f}) v0=({:.2f},{:.2f},{:.2f})", i, t, distance,
							r + hull.radius, hull.planeCount, plane.x, plane.y, plane.z, plane.w,
							model.hullTriangles[hull.firstTriangle].vertices[0].x, model.hullTriangles[hull.firstTriangle].vertices[0].y,
							model.hullTriangles[hull.firstTriangle].vertices[0].z);
						break;
					}
				}
			}
			for (std::size_t i = 0; i < model.capsules.size(); ++i) {
				const auto& capsule = model.capsules[i];
				if (Hlsl::CapsuleOverlapsCapsule(p, q, r, ToHlsl(capsule.p0), ToHlsl(capsule.p1), capsule.radius)) {
					kinds += std::format(" capsule#{}", i);
				}
			}
			out += std::format("[ref {:08X} base {:08X} {} scale {:.2f} hits:{}] ",
				instance.referenceFormID.value,
				instance.baseFormID.value,
				a_world.Models()[instance.model].path,
				instance.worldFromModel.scale,
				kinds);
		}
		return out;
	}

	std::vector<std::uint32_t> RejectCellOnCpu(const WorldIndex& a_world, const Grass::CellCandidates& a_cell, std::span<const QueryShape> a_shapes)
	{
		std::vector<std::uint32_t> rejected((a_cell.blades.size() + 31) / 32, 0u);
		const auto instances = a_world.InstancesInCell(a_cell.cellX, a_cell.cellY);
		if (instances.empty()) {
			return rejected;
		}
		for (std::size_t i = 0; i < a_cell.blades.size(); ++i) {
			const auto& blade = a_cell.blades[i];
			const auto& shape = a_shapes[blade.groupIndex];
			const auto [p, q] = BladeSegment(shape, blade.position);
			const Float3 lo{ p.x - shape.radius, p.y - shape.radius, p.z - shape.radius };
			const Float3 hi{ q.x + shape.radius, q.y + shape.radius, q.z + shape.radius };
			for (const auto instanceIndex : instances) {
				const auto& instance = a_world.Instances()[instanceIndex];
				if (lo.x > instance.aabbMax.x || hi.x < instance.aabbMin.x || lo.y > instance.aabbMax.y || hi.y < instance.aabbMin.y ||
					lo.z > instance.aabbMax.z || hi.z < instance.aabbMin.z) {
					continue;
				}
				if (CapsuleOverlapsInstance(a_world, instance, p, q, shape.radius)) {
					rejected[i / 32] |= 1u << (i % 32);
					break;
				}
			}
		}
		return rejected;
	}
}
