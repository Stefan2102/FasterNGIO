#include "Grass/Placement.h"
#include "Grass/SmoothPlacement.h"

#include <gtest/gtest.h>

#include <cmath>

namespace
{
	using namespace FasterNGIO;
	using GameData::FormID;
	using GameData::LandInfo;

	constexpr FormID kGrassTexture{ 0x200 };
	constexpr FormID kBareTexture{ 0x201 };

	GameData::GrassInfo MakeGrass(std::uint32_t a_formID, std::uint8_t a_density)
	{
		GameData::GrassInfo grass;
		grass.formID = FormID{ a_formID };
		grass.modelPath = "landscape\\grass\\test.nif";
		grass.density = a_density;
		grass.positionRange = 20.0f;
		return grass;
	}

	struct World
	{
		GameData::StaticWorldSnapshot snapshot;
		LandInfo land;

		World()
		{
			snapshot.grassesByFormID.emplace(FormID{ 0x100 }, MakeGrass(0x100, 50));
			GameData::LandTextureInfo grassy;
			grassy.formID = kGrassTexture;
			grassy.grassFormIDs = { FormID{ 0x100 } };
			snapshot.landTexturesByFormID.emplace(kGrassTexture, grassy);
			GameData::LandTextureInfo bare;
			bare.formID = kBareTexture;
			snapshot.landTexturesByFormID.emplace(kBareTexture, bare);

			land.cellX = 0;
			land.cellY = 0;
			land.hasHeights = true;
			SetBase(kGrassTexture);
		}

		void SetBase(FormID a_texture)
		{
			for (std::uint8_t q = 0; q < LandInfo::QuadrantCount; ++q) {
				land.baseTextures[q] = GameData::LandBaseTexture{ .landTextureFormID = a_texture, .quadrant = q };
			}
		}
	};

	Grass::PlacementSettings SmoothSettings()
	{
		Grass::PlacementSettings settings;
		settings.mode = Grass::PlacementMode::Smooth;
		settings.smooth.warpAmplitude = 0.0f;
		settings.smooth.densityBias = 1.0f;
		return settings;
	}
}

TEST(VanillaPlacement, TakesOneMoreGrassTypePerTextureThanTheIniMaximum)
{
	World world;
	// An unresolved GNAM first: the engine's list only holds resolved forms, so it does not count.
	auto& texture = world.snapshot.landTexturesByFormID.at(kGrassTexture);
	texture.grassFormIDs = { FormID{ 0x999 }, FormID{ 0x101 }, FormID{ 0x102 }, FormID{ 0x103 }, FormID{ 0x104 } };
	for (std::uint32_t id = 0x101; id <= 0x104; ++id) {
		world.snapshot.grassesByFormID.emplace(FormID{ id }, MakeGrass(id, 20));
	}

	Grass::PlacementSettings settings;
	settings.maxGrassTypesPerTexture = 2;
	const auto cell = Grass::GenerateCellCandidates(world.snapshot, world.land, settings);
	ASSERT_EQ(cell.groups.size(), 3u);
	EXPECT_EQ(cell.groups[0].grass->formID.value, 0x101u);
	EXPECT_EQ(cell.groups[1].grass->formID.value, 0x102u);
	EXPECT_EQ(cell.groups[2].grass->formID.value, 0x103u);
}

TEST(SmoothPlacement, IsDeterministic)
{
	World world;
	auto settings = SmoothSettings();
	settings.smooth.warpAmplitude = 96.0f;
	const auto a = Grass::GenerateCellCandidates(world.snapshot, world.land, settings);
	const auto b = Grass::GenerateCellCandidates(world.snapshot, world.land, settings);
	ASSERT_EQ(a.blades.size(), b.blades.size());
	ASSERT_FALSE(a.blades.empty());
	for (std::size_t i = 0; i < a.blades.size(); ++i) {
		ASSERT_EQ(a.blades[i].words, b.blades[i].words);
	}
}

TEST(SmoothPlacement, PlacesNothingOnTexturesWithoutGrass)
{
	World world;
	world.SetBase(kBareTexture);
	const auto cell = Grass::GenerateCellCandidates(world.snapshot, world.land, SmoothSettings());
	EXPECT_TRUE(cell.blades.empty());
}

TEST(SmoothPlacement, FullCoveragePlacesTheGrassDensity)
{
	World world;
	const auto cell = Grass::GenerateCellCandidates(world.snapshot, world.land, SmoothSettings());
	// iMinGrassSize 20 and position range 20: 25 lattice points per 512 units, 200 per cell side.
	const double expected = 0.5 * 200.0 * 200.0;
	EXPECT_NEAR(static_cast<double>(cell.blades.size()), expected, expected * 0.03);
	for (const auto& blade : cell.blades) {
		ASSERT_GE(blade.position[0], 0.0f);
		ASSERT_LE(blade.position[0], 4096.0f);
		ASSERT_GE(blade.position[1], 0.0f);
		ASSERT_LE(blade.position[1], 4096.0f);
	}
}

TEST(SmoothPlacement, AveragesVerticesSharedByQuadrants)
{
	World world;
	world.SetBase(kBareTexture);
	world.land.baseTextures[0] = GameData::LandBaseTexture{ .landTextureFormID = kGrassTexture, .quadrant = 0 };
	const auto grids = Grass::BuildSmoothWeightGrids(world.snapshot, world.land, SmoothSettings());
	ASSERT_EQ(grids.size(), 1u);
	const auto at = [&](std::size_t x, std::size_t y) { return grids[0].weights[y * LandInfo::VertexSide + x]; };
	EXPECT_EQ(at(8, 8), 255);    // inside quadrant 0
	EXPECT_EQ(at(16, 8), 128);   // shared by quadrants 0 and 1
	EXPECT_EQ(at(16, 16), 64);   // shared by all four
	EXPECT_EQ(at(24, 24), 0);    // inside quadrant 3
}

TEST(SmoothPlacement, DensityMatchKeepsFullCoverageAtTheGrassDensity)
{
	World world;
	auto settings = SmoothSettings();
	const std::vector<LandInfo> lands{ world.land };
	const Grass::SmoothWeightField field(world.snapshot, lands, settings);
	ASSERT_NE(field.Find(0, 0), nullptr);
	// Full coverage: vanilla and smooth expect the same count, so the scale stays at 1.
	EXPECT_NEAR(field.DensityScale(FormID{ 0x100 }), 1.0f, 0.02f);
	const auto& expected = field.Expected().at(FormID{ 0x100 });
	EXPECT_NEAR(expected.vanilla, 0.5 * 200.0 * 200.0, 1.0);
	EXPECT_NEAR(expected.smooth, 0.5 * 200.0 * 200.0, 200.0);
}

TEST(SmoothPlacement, DensityMatchCompensatesVanillasDoublePlacementInBlends)
{
	// Two textures carrying the same grass, blended 50/50 everywhere: vanilla places the grass once
	// per texture (double density); smooth caps the summed weight at full coverage, so the scale
	// makes up the difference.
	World world;
	GameData::LandTextureInfo second;
	second.formID = FormID{ 0x202 };
	second.grassFormIDs = { FormID{ 0x100 } };
	world.snapshot.landTexturesByFormID.emplace(second.formID, second);
	for (std::uint8_t q = 0; q < LandInfo::QuadrantCount; ++q) {
		world.land.alphaTextures.push_back(GameData::LandAlphaTexture{ .landTextureFormID = second.formID, .quadrant = q, .layerIndex = 0 });
		for (std::uint16_t v = 0; v < LandInfo::QuadrantVertexCount; ++v) {
			world.land.vertexAlphas.push_back(GameData::LandVertexAlpha{ .quadrant = q, .layerIndex = 0, .position = v, .opacity = 0.5f });
		}
	}
	auto settings = SmoothSettings();
	const std::vector<LandInfo> lands{ world.land };
	const Grass::SmoothWeightField field(world.snapshot, lands, settings);
	EXPECT_NEAR(field.DensityScale(FormID{ 0x100 }), 2.0f, 0.05f);
}
