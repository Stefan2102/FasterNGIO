#include "Collision/InstanceShape.h"
#include "Collision/NifCollisionExtractor.h"

#include <NifFile.hpp>
#include <bhk.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <sstream>
#include <vector>

using namespace FasterNGIO;

namespace
{
	// A cube of half-size a_half centred on a_center, as 12 triangles.
	void AddCube(nifly::NifFile& a_nif, const std::string& a_name, float a_half, float a_cx, uint32_t& a_shapeID)
	{
		std::vector<nifly::Vector3> vertices;
		for (int corner = 0; corner < 8; ++corner) {
			vertices.emplace_back(a_cx + ((corner & 1) ? a_half : -a_half), (corner & 2) ? a_half : -a_half, (corner & 4) ? a_half : -a_half);
		}
		const std::vector<nifly::Triangle> triangles{
			{ 0, 1, 3 }, { 0, 3, 2 }, { 4, 6, 7 }, { 4, 7, 5 }, { 0, 4, 5 }, { 0, 5, 1 },
			{ 2, 3, 7 }, { 2, 7, 6 }, { 0, 2, 6 }, { 0, 6, 4 }, { 1, 5, 7 }, { 1, 7, 3 },
		};
		auto* shape = a_nif.CreateShapeFromData(a_name, &vertices, &triangles, nullptr);
		a_shapeID = a_nif.GetHeader().GetBlockID(shape);
	}

	// A model with a visible cube, a decal cube, a hidden cube and, optionally, a box collider in
	// layer kStatic.
	[[nodiscard]] std::vector<std::uint8_t> MakeNif(bool a_collision)
	{
		nifly::NifFile nif;
		nif.Create(nifly::NiVersion::getSSE());
		auto& header = nif.GetHeader();
		std::uint32_t visible = 0;
		std::uint32_t decal = 0;
		std::uint32_t hidden = 0;
		AddCube(nif, "Rock", 10.0f, 0.0f, visible);
		AddCube(nif, "Moss", 10.0f, 100.0f, decal);
		AddCube(nif, "Marker", 10.0f, 200.0f, hidden);
		auto* decalShape = header.GetBlock<nifly::NiShape>(decal);
		auto* decalShader = dynamic_cast<nifly::BSLightingShaderProperty*>(nif.GetShader(decalShape));
		decalShader->shaderFlags1 |= nifly::SLSF1_DECAL;
		header.GetBlock<nifly::NiShape>(hidden)->flags |= 1u;

		if (a_collision) {
			auto box = std::make_unique<nifly::bhkBoxShape>();
			box->dimensions = nifly::Vector3(1.0f, 1.0f, 1.0f);
			const auto boxID = header.AddBlock(std::move(box));
			auto body = std::make_unique<nifly::bhkRigidBody>();
			body->shapeRef.index = boxID;
			body->collisionFilter.layer = 1;
			const auto bodyID = header.AddBlock(std::move(body));
			auto collision = std::make_unique<nifly::bhkCollisionObject>();
			collision->bodyRef.index = bodyID;
			nif.GetRootNode()->collisionRef.index = header.AddBlock(std::move(collision));
		}

		std::ostringstream stream(std::ios::binary);
		EXPECT_EQ(nif.Save(stream), 0);
		const auto bytes = stream.str();
		return { bytes.begin(), bytes.end() };
	}

	[[nodiscard]] Collision::ExtractionOptions RenderGeometry(std::uint32_t a_layerMask = Collision::kDefaultLayerMask)
	{
		Collision::ExtractionOptions options;
		options.layerMask = a_layerMask;
		options.renderGeometry = true;
		return options;
	}
}

TEST(Collision, RenderGeometryReplacesCollision)
{
	const auto bytes = MakeNif(true);

	const auto collision = Collision::ExtractCollision(bytes, {});
	ASSERT_EQ(collision.status, Collision::ExtractionStatus::HasCollision);
	EXPECT_EQ(collision.hulls.size(), 1u);
	EXPECT_TRUE(collision.triangles.empty());
	EXPECT_FALSE(collision.stats.renderGeometry);

	// Only the visible, non-decal cube replaces the collider.
	const auto render = Collision::ExtractCollision(bytes, RenderGeometry());
	ASSERT_EQ(render.status, Collision::ExtractionStatus::HasCollision);
	EXPECT_TRUE(render.stats.renderGeometry);
	EXPECT_TRUE(render.hulls.empty());
	EXPECT_EQ(render.triangles.size(), 12u);
	EXPECT_NEAR(render.aabbMin.x, -10.0f, 1.0e-4f);
	EXPECT_NEAR(render.aabbMax.x, 10.0f, 1.0e-4f);
	EXPECT_NEAR(render.aabbMax.z, 10.0f, 1.0e-4f);
}

TEST(Collision, RenderGeometryNeedsCollision)
{
	// A model without collision keeps grass, whatever its render shapes.
	const auto model = Collision::ExtractCollision(MakeNif(false), RenderGeometry());
	EXPECT_EQ(model.status, Collision::ExtractionStatus::NoCollision);
	EXPECT_TRUE(model.triangles.empty());
}

TEST(Collision, RenderGeometryRespectsTheLayerMask)
{
	// Collision outside the layer mask does not reject, so neither does the render geometry.
	const auto model = Collision::ExtractCollision(MakeNif(true), RenderGeometry(1u << 4));
	EXPECT_EQ(model.status, Collision::ExtractionStatus::FilteredOut);
	EXPECT_TRUE(model.triangles.empty());
}

TEST(Collision, InstanceShapeIsTheRootsFirstChild)
{
	// The first cube: 12 triangles over 8 vertices.
	const auto counts = Collision::ReadInstanceShapeCounts(MakeNif(false));
	ASSERT_TRUE(counts.has_value());
	EXPECT_EQ(counts->triangles, 12u);
	EXPECT_EQ(counts->vertices, 8u);
}

TEST(Collision, InstanceShapeNeedsAChildShape)
{
	nifly::NifFile nif;
	nif.Create(nifly::NiVersion::getSSE());
	std::ostringstream stream(std::ios::binary);
	ASSERT_EQ(nif.Save(stream), 0);
	const auto bytes = stream.str();
	EXPECT_FALSE(Collision::ReadInstanceShapeCounts(std::vector<std::uint8_t>(bytes.begin(), bytes.end())).has_value());
	const std::vector<std::uint8_t> garbage{ 1, 2, 3 };
	EXPECT_FALSE(Collision::ReadInstanceShapeCounts(garbage).has_value());
}
