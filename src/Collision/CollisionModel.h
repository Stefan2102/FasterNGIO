#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace FasterNGIO::Collision
{
	// Skyrim's Havok world scale: one Havok unit is 1 / 0.0142875 game units.
	inline constexpr float kHavokToGame = 1.0f / 0.0142875f;

	struct Float3
	{
		float x{ 0.0f };
		float y{ 0.0f };
		float z{ 0.0f };
	};

	struct Float4
	{
		float x{ 0.0f };
		float y{ 0.0f };
		float z{ 0.0f };
		float w{ 0.0f };
	};

	// A mesh-shape triangle (bhkCompressedMeshShape, bhkPackedNiTriStripsShape). Mesh shapes are
	// surfaces: a query volume entirely inside a closed mesh does not touch it, as in Havok.
	struct Triangle
	{
		std::array<Float3, 3> vertices{};
		float radius{ 0.0f };
	};

	// A solid convex hull (bhkConvexVerticesShape, bhkBoxShape). Planes satisfy dot(n, p) + w <= 0
	// inside; the face triangulation is used for distance queries.
	struct Hull
	{
		std::uint32_t firstPlane{ 0 };
		std::uint32_t planeCount{ 0 };
		std::uint32_t firstTriangle{ 0 };
		std::uint32_t triangleCount{ 0 };
		float radius{ 0.0f };
		Float3 aabbMin;
		Float3 aabbMax;
	};

	// A solid capsule (bhkCapsuleShape, bhkSphereShape and bhkMultiSphereShape spheres, where p0 == p1).
	struct Capsule
	{
		Float3 p0;
		Float3 p1;
		float radius{ 0.0f };
	};

	enum class ShapeType : std::uint32_t
	{
		CompressedMesh,
		PackedStrips,
		ConvexVertices,
		Box,
		Sphere,
		Capsule,
		MultiSphere,
		List,
		ConvexList,
		Transform,
		Mopp,
		Unsupported,
		Count
	};

	struct ExtractionStats
	{
		std::uint32_t collisionObjects{ 0 };
		std::uint32_t bodiesKept{ 0 };
		std::uint32_t bodiesFilteredByLayer{ 0 };
		std::uint32_t nonRigidBodies{ 0 };
		std::array<std::uint32_t, static_cast<std::size_t>(ShapeType::Count)> shapes{};
		std::vector<std::string> unsupportedShapes;
		std::vector<std::string> trace;
	};

	enum class ExtractionStatus : std::uint8_t
	{
		// The model has collision in a layer that rejects grass.
		HasCollision,
		// The model has collision objects, but none in a rejecting layer.
		FilteredOut,
		// The model has no collision objects at all (render-geometry fallback candidate).
		NoCollision,
		// The model could not be found or parsed.
		LoadFailed
	};

	// Collision of one model in model space (game units, before the reference transform). The
	// NIF root's own transform is not applied: the engine replaces it with the reference's.
	struct CollisionModel
	{
		ExtractionStatus status{ ExtractionStatus::NoCollision };
		std::vector<Triangle> triangles;
		std::vector<Hull> hulls;
		std::vector<Float4> hullPlanes;
		std::vector<Triangle> hullTriangles;
		std::vector<Capsule> capsules;
		Float3 aabbMin;
		Float3 aabbMax;
		// Bounds of the render shapes, when requested; empty (min > max) when there are none.
		Float3 renderAabbMin{ 1.0f, 1.0f, 1.0f };
		Float3 renderAabbMax{ -1.0f, -1.0f, -1.0f };
		ExtractionStats stats;

		[[nodiscard]] bool Empty() const { return triangles.empty() && hulls.empty() && capsules.empty(); }
	};
}
