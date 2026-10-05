#include "GameData/Records.h"
#include "Rejection/CpuBvh.h"
#include "Rejection/CpuReference.h"
#include "Rejection/NgioRules.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace
{
	using namespace FasterNGIO;
	using Collision::Float3;
	using Rejection::CliffOutcome;

	// A horizontal square of two mesh triangles at height a_z, a_half units on each side of the origin.
	Collision::CollisionModel Slab(float a_half, float a_z)
	{
		Collision::CollisionModel model;
		model.status = Collision::ExtractionStatus::HasCollision;
		const Float3 a{ -a_half, -a_half, a_z };
		const Float3 b{ a_half, -a_half, a_z };
		const Float3 c{ a_half, a_half, a_z };
		const Float3 d{ -a_half, a_half, a_z };
		model.triangles.push_back(Collision::Triangle{ .vertices = { a, b, c }, .radius = 0.0f });
		model.triangles.push_back(Collision::Triangle{ .vertices = { a, c, d }, .radius = 0.0f });
		model.aabbMin = { -a_half, -a_half, a_z };
		model.aabbMax = { a_half, a_half, a_z };
		return model;
	}

	struct Placed
	{
		Collision::CollisionModel model;
		float x{ 0.0f };
		float y{ 0.0f };
		Rejection::InstanceRole role{ Rejection::kRoleCliff };
		GameData::FormID base{};
	};

	Rejection::WorldIndex MakeWorld(std::vector<Placed> a_objects)
	{
		std::vector<Rejection::ModelRecord> models;
		std::vector<Rejection::PlacedModel> placements;
		for (auto& object : a_objects) {
			const float position[3]{ object.x, object.y, 0.0f };
			const float rotation[3]{ 0.0f, 0.0f, 0.0f };
			auto& placement = placements.emplace_back();
			placement.model = static_cast<std::uint32_t>(models.size());
			placement.worldFromModel = Rejection::Similarity::FromPlacement(position, rotation, 1.0f);
			placement.role = object.role;
			placement.baseFormID = object.base;
			models.push_back(Rejection::ModelRecord{ .path = "model.nif", .collision = std::move(object.model) });
		}
		return Rejection::WorldIndex(std::move(models), placements, 50.0f);
	}

	GameData::GrassInfo AnyGrass()
	{
		GameData::GrassInfo grass;
		grass.maxSlopeDegrees = 90;
		return grass;
	}

	Rejection::RejectionFeatures Cliffs()
	{
		Rejection::RejectionFeatures features;
		features.cliffs = true;
		return features;
	}

	// A blade at (x, y, 0) with NGIO's default capsule, judged on the CPU BVH.
	Rejection::BladeDecision Judge(const Rejection::WorldIndex& a_world, float a_x, float a_y, const Rejection::RejectionFeatures& a_features = Cliffs())
	{
		const Rejection::CpuBvh bvh(a_world);
		const float position[3]{ a_x, a_y, 0.0f };
		const auto hits = bvh.ClassifyCapsule(Float3{ a_x, a_y, -5.0f }, Float3{ a_x, a_y, 150.0f }, 10.0f);
		Rejection::CliffRays rays;
		if (Rejection::NeedsCliffRays(hits, a_features)) {
			rays = Rejection::TraceCliffRays([&](const Float3& a_p, const Float3& a_q, std::vector<Rejection::WorldSegmentHit>& a_hits) { bvh.SegmentHitsWorld(a_p, a_q, a_hits); },
				a_world, position, Rejection::CliffNeighbourOffset(AnyGrass()));
		}
		return Rejection::DecideBlade(hits, rays, a_world, a_features, AnyGrass(), position);
	}
}

TEST(GrassCliffs, MoveABladeUnderACliffOntoIt)
{
	const auto world = MakeWorld({ Placed{ .model = Slab(300.0f, 100.0f) } });
	const auto decision = Judge(world, 0.0f, 0.0f);
	EXPECT_FALSE(decision.rejected);
	ASSERT_TRUE(decision.moved);
	EXPECT_NEAR(decision.z, 100.0f, 1.0e-3f);
	EXPECT_NEAR(decision.normal.z, 1.0f, 1.0e-5f);

	// The same object without the cliff role rejects the blade, and so do cliffs when they are off.
	EXPECT_TRUE(Judge(MakeWorld({ Placed{ .model = Slab(300.0f, 100.0f), .role = Rejection::kRoleOrdinary } }), 0.0f, 0.0f).rejected);
	Rejection::RejectionFeatures off;
	EXPECT_TRUE(Judge(world, 0.0f, 0.0f, off).rejected);
}

TEST(GrassCliffs, RejectWhenTheNeighboursMissTheCliff)
{
	// Too small for the neighbours 80 units away: the placement fails, and the blade is rejected.
	const auto decision = Judge(MakeWorld({ Placed{ .model = Slab(50.0f, 100.0f) } }), 0.0f, 0.0f);
	EXPECT_TRUE(decision.rejected);
	EXPECT_FALSE(decision.moved);
}

TEST(GrassCliffs, IgnoreACliffTheRayUpMisses)
{
	// The capsule (radius 10) grazes the slab's edge 5 units away; the ray up misses it.
	const auto world = MakeWorld({ Placed{ .model = Slab(300.0f, 100.0f), .x = 305.0f } });
	const auto decision = Judge(world, 0.0f, 0.0f);
	EXPECT_FALSE(decision.rejected);
	EXPECT_FALSE(decision.moved);
}

TEST(GrassCliffs, AreNotCliffsWhenSomethingElseIsNearerAbove)
{
	// An ordinary object between the blade and the cliff (both within the capsule): the ray up meets
	// it first, so the cliff is ignored and the object rejects.
	const auto world = MakeWorld({
		Placed{ .model = Slab(300.0f, 140.0f) },
		Placed{ .model = Slab(20.0f, 120.0f), .role = Rejection::kRoleOrdinary },
	});
	const auto decision = Judge(world, 0.0f, 0.0f);
	EXPECT_TRUE(decision.rejected);
	EXPECT_FALSE(decision.moved);
}

TEST(GrassCliffs, CheckTheSlopeAndTheSteepWindow)
{
	const auto world = MakeWorld({ Placed{ .model = Slab(300.0f, 100.0f) } });
	const float position[3]{ 0.0f, 0.0f, 0.0f };
	Rejection::CliffRays rays;
	rays.upClosest = 0;
	rays.cliffT = 0.25f;
	// 60 degrees from vertical.
	rays.cliffNormal = { std::sin(1.0472f), 0.0f, std::cos(1.0472f) };
	for (auto& neighbour : rays.neighbours) {
		neighbour = Rejection::CliffRay{ .closest = 0, .highestT = 0.5f };
	}
	auto grass = AnyGrass();
	EXPECT_EQ(Rejection::DecideCliff(rays, world, Cliffs(), grass, position).outcome, CliffOutcome::Moved);
	grass.maxSlopeDegrees = 45;
	EXPECT_EQ(Rejection::DecideCliff(rays, world, Cliffs(), grass, position).outcome, CliffOutcome::Failed);
	grass.maxSlopeDegrees = 90;
	grass.minSlopeDegrees = 70;
	EXPECT_EQ(Rejection::DecideCliff(rays, world, Cliffs(), grass, position).outcome, CliffOutcome::Failed);

	// Grass that grows only under water is left where it is.
	grass = AnyGrass();
	grass.underwaterState = GameData::GrassWaterState::BelowOnlyAtLeast;
	EXPECT_EQ(Rejection::DecideCliff(rays, world, Cliffs(), grass, position).outcome, CliffOutcome::NotCliff);

	// Steep cliffs look 80 units up and down for their neighbours instead of 30.
	const auto normal = Rejection::MakeCliffNeighbourSegments(position, 150.0f, 100.0f, false);
	const auto steep = Rejection::MakeCliffNeighbourSegments(position, 150.0f, 100.0f, true);
	EXPECT_FLOAT_EQ(normal.bottom[0].z, 120.0f);
	EXPECT_FLOAT_EQ(steep.top[0].z, 230.0f);
	EXPECT_FLOAT_EQ(steep.bottom[3].y, -100.0f);
}

TEST(GrassCliffs, NeighbourOffsetFollowsTheGrassBounds)
{
	auto grass = AnyGrass();
	EXPECT_FLOAT_EQ(Rejection::CliffNeighbourOffset(grass), 80.0f);
	grass.bounds.present = true;
	grass.bounds.min = { -30, -10, 0 };
	grass.bounds.max = { 30, 50, 40 };
	EXPECT_FLOAT_EQ(Rejection::CliffNeighbourOffset(grass), 30.0f + 40.0f);
}

TEST(GrassCliffs, TheBruteForceReferenceAgrees)
{
	const auto world = MakeWorld({
		Placed{ .model = Slab(300.0f, 100.0f) },
		Placed{ .model = Slab(40.0f, 60.0f), .x = 400.0f, .role = Rejection::kRoleOrdinary },
		Placed{ .model = Slab(300.0f, 100.0f), .x = 900.0f, .y = 300.0f },
	});
	const Rejection::CpuBvh bvh(world);
	for (float y = -350.0f; y < 650.0f; y += 23.0f) {
		for (float x = -350.0f; x < 1250.0f; x += 29.0f) {
			const Float3 p{ x, y, -5.0f };
			const Float3 q{ x, y, 150.0f };
			const auto fast = bvh.ClassifyCapsule(p, q, 10.0f);
			const auto slow = Rejection::ClassifyCapsuleBruteForce(world, 0, 0, p, q, 10.0f);
			// The BVH stops at an ordinary hit only when no cliff could follow, so both agree in full.
			EXPECT_EQ(fast.ordinary, slow.ordinary) << x << ", " << y;
			EXPECT_EQ(fast.cliff, slow.cliff) << x << ", " << y;
		}
	}
}

TEST(IgnoredShapes, KeepGrassNearTheNamedShapesOnly)
{
	// A slab whose render mesh has a "Curb" shape along its -x edge and a "Body" shape in the middle.
	auto model = Slab(300.0f, 100.0f);
	model.rootName = "Road01";
	model.renderShapes.push_back(Collision::RenderShape{ .name = "Road01:Curb", .vertices = { Float3{ -280, 0, 100 }, Float3{ -280, 200, 100 } } });
	model.renderShapes.push_back(Collision::RenderShape{ .name = "Body", .vertices = { Float3{ 0, 0, 100 }, Float3{ 100, 0, 100 } } });
	constexpr GameData::FormID kRoad{ 0x800 };
	const auto world = MakeWorld({ Placed{ .model = std::move(model), .role = Rejection::kRolePartIgnored, .base = kRoad } });
	Rejection::RejectionFeatures features;
	features.ignoredShapes[kRoad] = { "Curb" };

	// The "<root>:<name>" form of the shape's name matches too.
	EXPECT_FALSE(Judge(world, -250.0f, 50.0f, features).rejected);
	EXPECT_TRUE(Judge(world, 20.0f, 0.0f, features).rejected);
	// Without the entry, the object rejects like any other.
	EXPECT_TRUE(Judge(world, -250.0f, 50.0f, Rejection::RejectionFeatures{}).rejected);
}
