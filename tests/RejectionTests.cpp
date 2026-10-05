#include "GameData/Records.h"
#include "Gpu/ModelPacking.h"
#include "Grass/Placement.h"
#include "Rejection/CpuBvh.h"
#include "Rejection/CpuReference.h"
#include "Rejection/RejectionConfig.h"
#include "Rejection/WorldIndex.h"

#include <gtest/gtest.h>

#include <bit>
#include <cstring>
#include <vector>

namespace
{
	using namespace FasterNGIO;
	using Collision::Float3;

	// One model with each primitive kind: a box hull at the origin, a wall triangle and a capsule.
	Collision::CollisionModel MakeModel()
	{
		Collision::CollisionModel model;
		model.status = Collision::ExtractionStatus::HasCollision;

		// Box [-50, 50] x [-50, 50] x [0, 100]: planes face outward (dot(n, p) + w <= 0 inside).
		const Float3 lo{ -50, -50, 0 };
		const Float3 hi{ 50, 50, 100 };
		model.hullPlanes = { { 1, 0, 0, -hi.x }, { -1, 0, 0, lo.x }, { 0, 1, 0, -hi.y }, { 0, -1, 0, lo.y }, { 0, 0, 1, -hi.z }, { 0, 0, -1, lo.z } };
		const auto corner = [&](int a_index) { return Float3{ (a_index & 1) ? hi.x : lo.x, (a_index & 2) ? hi.y : lo.y, (a_index & 4) ? hi.z : lo.z }; };
		constexpr int kFaces[12][3] = { { 0, 1, 3 }, { 0, 3, 2 }, { 4, 6, 7 }, { 4, 7, 5 }, { 0, 4, 5 }, { 0, 5, 1 }, { 2, 3, 7 }, { 2, 7, 6 }, { 0, 2, 6 }, { 0, 6, 4 },
			{ 1, 5, 7 }, { 1, 7, 3 } };
		for (const auto& face : kFaces) {
			model.hullTriangles.push_back(Collision::Triangle{ .vertices = { corner(face[0]), corner(face[1]), corner(face[2]) }, .radius = 0.0f });
		}
		model.hulls.push_back(Collision::Hull{ .firstPlane = 0, .planeCount = 6, .firstTriangle = 0, .triangleCount = 12, .radius = 1.0f, .aabbMin = lo, .aabbMax = hi });

		model.triangles.push_back(Collision::Triangle{ .vertices = { Float3{ -200, -100, 0 }, Float3{ -200, 100, 0 }, Float3{ -200, 0, 200 } }, .radius = 2.0f });
		model.capsules.push_back(Collision::Capsule{ .p0 = { 200, 0, 0 }, .p1 = { 200, 0, 100 }, .radius = 20.0f });
		model.aabbMin = { -200, -100, 0 };
		model.aabbMax = { 220, 100, 200 };
		return model;
	}

	// Three placements of the model in cell (0, 0): plain, rotated and scaled, and tilted.
	Rejection::WorldIndex MakeWorld(float a_maxReach)
	{
		std::vector<Rejection::ModelRecord> models(1);
		models[0].collision = MakeModel();
		const auto place = [](float a_x, float a_y, float a_z, float a_rx, float a_ry, float a_rz, float a_scale) {
			const float position[3]{ a_x, a_y, a_z };
			const float rotation[3]{ a_rx, a_ry, a_rz };
			return Rejection::PlacedModel{ .model = 0, .worldFromModel = Rejection::Similarity::FromPlacement(position, rotation, a_scale) };
		};
		const std::vector<Rejection::PlacedModel> placements{
			place(1000, 1000, 0, 0, 0, 0, 1.0f),
			place(2500, 1200, 10, 0, 0, 0.7f, 1.5f),
			place(1800, 3000, -20, 0.1f, 0.2f, 1.3f, 0.8f),
		};
		return Rejection::WorldIndex(std::move(models), placements, a_maxReach);
	}

	// A grid of blades over the cell, all of one grass type.
	Grass::CellCandidates MakeBlades()
	{
		Grass::CellCandidates cell;
		cell.groups.push_back(Grass::CellGrassGroup{});
		for (int y = 0; y < 80; ++y) {
			for (int x = 0; x < 80; ++x) {
				auto& blade = cell.blades.emplace_back();
				blade.position[0] = 50.0f + static_cast<float>(x) * 51.0f;
				blade.position[1] = 50.0f + static_cast<float>(y) * 51.0f;
				blade.position[2] = static_cast<float>((x * 7 + y * 13) % 120);
			}
		}
		return cell;
	}
}

TEST(RejectionConfig, SizesShapesAsNgioDoes)
{
	Rejection::RejectionConfig config;
	GameData::GrassInfo grass;
	// Without bounds: NGIO's 20-unit default, times the width multiplier.
	auto shape = Rejection::MakeQueryShape(config, grass);
	EXPECT_FLOAT_EQ(shape.depth, 5.0f);
	EXPECT_FLOAT_EQ(shape.height, 150.0f);
	EXPECT_FLOAT_EQ(shape.radius, 20.0f * 0.3f);

	// From the bounds: half the larger half-extent.
	grass.bounds = GameData::ObjectBounds{ .min = { -10, -20, 0 }, .max = { 10, 20, 30 }, .present = true };
	shape = Rejection::MakeQueryShape(config, grass);
	EXPECT_FLOAT_EQ(shape.radius, 20.0f * 0.5f * 0.3f);

	// An explicit width wins.
	config.rayWidth = 8.0f;
	EXPECT_FLOAT_EQ(Rejection::MakeQueryShape(config, grass).radius, 4.0f);

	config.mode = Rejection::QueryMode::Ray;
	EXPECT_FLOAT_EQ(Rejection::MakeQueryShape(config, grass).radius, 0.0f);
}

TEST(CpuBvh, AgreesWithTheBruteForceReference)
{
	const std::vector<Rejection::QueryShape> shapes{ Rejection::QueryShape{ .depth = 5.0f, .height = 150.0f, .radius = 6.0f } };
	const auto world = MakeWorld(shapes[0].radius);
	ASSERT_EQ(world.Instances().size(), 3u);
	const Rejection::CpuBvh bvh(world);
	const auto cell = MakeBlades();

	const auto reference = Rejection::RejectCellOnCpu(world, cell, shapes);
	const auto fast = bvh.RejectCell(cell, shapes);
	ASSERT_EQ(reference, fast);
	std::size_t hits = 0;
	for (const auto word : fast) {
		hits += static_cast<std::size_t>(std::popcount(word));
	}
	// Some blades in each instance's footprint, most of the cell clear.
	EXPECT_GT(hits, 5u);
	EXPECT_LT(hits, cell.blades.size() / 4);
}

TEST(CpuBvh, FindsEachPrimitiveKind)
{
	const auto world = MakeWorld(0.0f);
	const Rejection::CpuBvh bvh(world);
	// The plain instance sits at (1000, 1000, 0): its box, wall and capsule, and empty ground.
	EXPECT_TRUE(bvh.CapsuleHitsWorld({ 1000, 1000, -5 }, { 1000, 1000, 150 }, 0.0f));
	EXPECT_TRUE(bvh.CapsuleHitsWorld({ 800, 1000, -5 }, { 800, 1000, 150 }, 0.0f));
	EXPECT_TRUE(bvh.CapsuleHitsWorld({ 1215, 1000, -5 }, { 1215, 1000, 150 }, 0.0f));
	EXPECT_FALSE(bvh.CapsuleHitsWorld({ 1100, 1300, -5 }, { 1100, 1300, 150 }, 5.0f));
	// The query radius reaches the capsule from outside it.
	EXPECT_FALSE(bvh.CapsuleHitsWorld({ 1250, 1000, -5 }, { 1250, 1000, 150 }, 5.0f));
	EXPECT_TRUE(bvh.CapsuleHitsWorld({ 1250, 1000, -5 }, { 1250, 1000, 150 }, 35.0f));
}

TEST(ModelPacking, WritesTheHeaderAndOneGeometryPerKind)
{
	auto model = MakeModel();
	model.hulls.clear();
	const auto packed = Gpu::PackModel(model, 3.0f);
	ASSERT_EQ(packed.geometries.size(), 2u);
	EXPECT_EQ(packed.geometries[0].kind, Rejection::PrimitiveKind::Triangle);
	EXPECT_EQ(packed.geometries[1].kind, Rejection::PrimitiveKind::Capsule);
	EXPECT_EQ(packed.kindMask, 0b101u);

	const auto word = [&](std::size_t a_offset) {
		std::uint32_t value = 0;
		std::memcpy(&value, packed.bytes.data() + a_offset, sizeof(value));
		return value;
	};
	EXPECT_EQ(word(4), 1u);   // triangle count
	EXPECT_EQ(word(12), 0u);  // hull count
	EXPECT_EQ(word(20), 1u);  // capsule count

	// The capsule's AABB grows by its radius plus the inflation.
	Rejection::Bounds bounds;
	std::memcpy(&bounds, packed.bytes.data() + packed.geometries[1].aabbOffset, sizeof(bounds));
	EXPECT_FLOAT_EQ(bounds.min[0], 200.0f - 23.0f);
	EXPECT_FLOAT_EQ(bounds.max[2], 100.0f + 23.0f);
}
