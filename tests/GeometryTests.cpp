#include "Rejection/HlslShim.h"
#include "Rejection/WorldIndex.h"

#include <gtest/gtest.h>

#include <numbers>

namespace
{
	using namespace FasterNGIO::Rejection;
	using Hlsl::float3;

	constexpr float3 A{ 0, 0, 0 };
	constexpr float3 B{ 10, 0, 0 };
	constexpr float3 C{ 0, 10, 0 };
}

TEST(QueryMath, PointTriangleDistance)
{
	EXPECT_FLOAT_EQ(Hlsl::PointTriangleDistanceSq({ 2, 2, 5 }, A, B, C), 25.0f);
	EXPECT_FLOAT_EQ(Hlsl::PointTriangleDistanceSq({ -3, 0, 4 }, A, B, C), 25.0f);
	EXPECT_FLOAT_EQ(Hlsl::PointTriangleDistanceSq({ 10, 10, 0 }, A, B, C), 50.0f);
}

TEST(QueryMath, SegmentTriangle)
{
	// A vertical segment piercing the triangle.
	EXPECT_FLOAT_EQ(Hlsl::SegmentTriangleDistanceSq({ 2, 2, -5 }, { 2, 2, 5 }, A, B, C), 0.0f);
	// A vertical segment beside the hypotenuse: closest feature is the edge.
	const auto d = Hlsl::SegmentTriangleDistanceSq({ 10, 10, -5 }, { 10, 10, 5 }, A, B, C);
	EXPECT_NEAR(d, 50.0f, 1e-3f);
	// Hovering above: endpoint distance.
	EXPECT_FLOAT_EQ(Hlsl::SegmentTriangleDistanceSq({ 2, 2, 3 }, { 2, 2, 8 }, A, B, C), 9.0f);
}

TEST(QueryMath, CapsuleTriangleRadius)
{
	EXPECT_TRUE(Hlsl::CapsuleOverlapsTriangle({ 2, 2, 3 }, { 2, 2, 8 }, 2.5f, A, B, C, 0.5f));
	EXPECT_FALSE(Hlsl::CapsuleOverlapsTriangle({ 2, 2, 3 }, { 2, 2, 8 }, 2.4f, A, B, C, 0.5f));
}

TEST(QueryMath, SegmentSegment)
{
	EXPECT_FLOAT_EQ(Hlsl::SegmentSegmentDistanceSq({ 0, 0, 0 }, { 0, 0, 10 }, { -5, 3, 5 }, { 5, 3, 5 }), 9.0f);
	EXPECT_FLOAT_EQ(Hlsl::SegmentSegmentDistanceSq({ 0, 0, 0 }, { 0, 0, 10 }, { 4, 0, 20 }, { 4, 0, 20 }), 16.0f + 100.0f);
}

TEST(Similarity, SkyrimZRotationIsClockwise)
{
	const float position[3]{ 100, 200, 300 };
	const float rotation[3]{ 0, 0, std::numbers::pi_v<float> / 2 };
	const auto transform = Similarity::FromPlacement(position, rotation, 2.0f);
	const auto x = transform.Apply({ 1, 0, 0 });
	EXPECT_NEAR(x.x, 100.0f, 1e-4f);
	EXPECT_NEAR(x.y, 198.0f, 1e-4f);
	EXPECT_NEAR(x.z, 300.0f, 1e-4f);
}

TEST(Similarity, InverseRoundTrips)
{
	const float position[3]{ -4096, 123, 77 };
	const float rotation[3]{ 0.3f, -1.1f, 2.4f };
	const auto transform = Similarity::FromPlacement(position, rotation, 1.7f);
	const auto inverse = transform.Inverse();
	const auto back = inverse.Apply(transform.Apply({ 3, -9, 27 }));
	EXPECT_NEAR(back.x, 3.0f, 1e-3f);
	EXPECT_NEAR(back.y, -9.0f, 1e-3f);
	EXPECT_NEAR(back.z, 27.0f, 1e-3f);
	EXPECT_NEAR(inverse.scale, 1.0f / 1.7f, 1e-6f);
}
