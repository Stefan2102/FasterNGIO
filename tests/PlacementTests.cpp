#include "Grass/Placement.h"
#include "Grass/Internal/PlacementCommon.h"
#include "Grass/LandTexture.h"
#include "Grass/SmoothPlacement.h"

#include <gtest/gtest.h>

#include <array>
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

TEST(VanillaPlacement, CountsButSkipsGrassWhoseModelCannotLoad)
{
	World world;
	auto& texture = world.snapshot.landTexturesByFormID.at(kGrassTexture);
	texture.grassFormIDs = { FormID{ 0x101 }, FormID{ 0x102 }, FormID{ 0x103 }, FormID{ 0x104 } };
	for (std::uint32_t id = 0x101; id <= 0x104; ++id) {
		world.snapshot.grassesByFormID.emplace(FormID{ id }, MakeGrass(id, 20));
	}
	Grass::PlacementSettings settings;
	settings.maxGrassTypesPerTexture = 2;
	const std::unordered_set<FormID, GameData::FormIDHash> unloadable{ FormID{ 0x102 } };
	settings.unloadableGrass = &unloadable;
	const auto cell = Grass::GenerateCellCandidates(world.snapshot, world.land, settings);
	// 0x102 still takes one of the three places, so 0x104 is not reached.
	ASSERT_EQ(cell.groups.size(), 2u);
	EXPECT_EQ(cell.groups[0].grass->formID.value, 0x101u);
	EXPECT_EQ(cell.groups[1].grass->formID.value, 0x103u);

	// Exactly as for a GRAS without a model: no blades and no RNG draws for it.
	auto noModel = world;
	noModel.snapshot.grassesByFormID.at(FormID{ 0x102 }).modelPath.clear();
	settings.unloadableGrass = nullptr;
	const auto reference = Grass::GenerateCellCandidates(noModel.snapshot, noModel.land, settings);
	ASSERT_EQ(reference.blades.size(), cell.blades.size());
	for (std::size_t i = 0; i < cell.blades.size(); ++i) {
		EXPECT_EQ(reference.blades[i].words, cell.blades[i].words);
	}
}

TEST(VanillaPlacement, GrowsTheSeasonsSwappedGrassList)
{
	World world;
	world.SetBase(kBareTexture);
	Grass::PlacementSettings settings;
	EXPECT_TRUE(Grass::GenerateCellCandidates(world.snapshot, world.land, settings).blades.empty());

	// Seasons of Skyrim's GetGrassList: the bare texture takes the grassy one's grass list, and the
	// grassy one takes the bare one's.
	const std::unordered_map<FormID, FormID, GameData::FormIDHash> swap{ { kBareTexture, kGrassTexture }, { kGrassTexture, kBareTexture } };
	settings.landTextureGrass = &swap;
	const auto swapped = Grass::GenerateCellCandidates(world.snapshot, world.land, settings);
	world.SetBase(kGrassTexture);
	settings.landTextureGrass = nullptr;
	const auto grassy = Grass::GenerateCellCandidates(world.snapshot, world.land, settings);
	ASSERT_FALSE(grassy.blades.empty());
	ASSERT_EQ(swapped.blades.size(), grassy.blades.size());
	for (std::size_t i = 0; i < grassy.blades.size(); ++i) {
		ASSERT_EQ(swapped.blades[i].words, grassy.blades[i].words);
	}
	settings.landTextureGrass = &swap;
	EXPECT_TRUE(Grass::GenerateCellCandidates(world.snapshot, world.land, settings).blades.empty());
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

namespace
{
	// The instance rotation as the game's grass vertex shader builds it from a blade's words:
	// float3x3(words 4-6, words 8-10, (word 12, word 7, word 11)), rows first.
	std::array<std::array<float, 3>, 3> InstanceMatrix(const Grass::BladeCandidate& a_blade)
	{
		const auto h = [&](std::size_t a_word) { return Grass::Internal::HalfBitsToFloat(a_blade.words[a_word]); };
		return { { { h(4), h(5), h(6) }, { h(8), h(9), h(10) }, { h(12), h(7), h(11) } } };
	}

	void ExpectRotationWithUp(const Grass::BladeCandidate& a_blade, const std::array<float, 3>& a_up)
	{
		const auto m = InstanceMatrix(a_blade);
		constexpr float kHalfError = 2.0e-3f;
		for (int row = 0; row < 3; ++row) {
			// The model's up axis goes to a_up: the matrix's third column.
			EXPECT_NEAR(m[row][2], a_up[row], kHalfError) << "row " << row;
			for (int other = 0; other < 3; ++other) {
				const float dot = m[row][0] * m[other][0] + m[row][1] * m[other][1] + m[row][2] * m[other][2];
				EXPECT_NEAR(dot, row == other ? 1.0f : 0.0f, kHalfError) << "rows " << row << ", " << other;
			}
		}
		const float det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
		                  m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
		EXPECT_NEAR(det, 1.0f, kHalfError);
	}
}

TEST(BladeEncoding, StandsUprightInTheGamesInstanceLayout)
{
	auto grass = MakeGrass(0x100, 50);
	Grass::Internal::TerrainSample terrain;
	terrain.normal[0] = 0.6f;
	terrain.normal[2] = 0.8f;
	for (const float orientation : { -0.9f, 0.0f, 0.4f }) {
		Grass::BladeCandidate blade;
		Grass::Internal::EncodeBlade(blade, 0, 0, 100.0f, 200.0f, terrain, grass, 0.5f, orientation, 0.0f, grass.FitsToSlope());
		ExpectRotationWithUp(blade, { 0.0f, 0.0f, 1.0f });
	}
}

TEST(BladeEncoding, FitToSlopeStandsOnTheTerrainNormal)
{
	auto grass = MakeGrass(0x100, 50);
	grass.grassFlags = 0x4;
	Grass::Internal::TerrainSample terrain;
	terrain.normal[0] = 0.6f;
	terrain.normal[2] = 0.8f;
	for (const float orientation : { -0.9f, 0.0f, 0.4f }) {
		Grass::BladeCandidate blade;
		Grass::Internal::EncodeBlade(blade, 0, 0, 100.0f, 200.0f, terrain, grass, 0.5f, orientation, 0.0f, grass.FitsToSlope());
		ExpectRotationWithUp(blade, { 0.6f, 0.0f, 0.8f });
	}
}

TEST(LandTextureMask, PicksTheStrongestTextureAtTheNearestVertex)
{
	World world;
	// The top-right quadrant's base texture is bare; the bottom-left one carries a bare alpha layer
	// at its vertex (4, 4) (opacity 0.8 beats the base's remaining 0.2) and a weak one at (8, 8).
	world.land.baseTextures[3] = GameData::LandBaseTexture{ .landTextureFormID = kBareTexture, .quadrant = 3 };
	world.land.alphaTextures.push_back(GameData::LandAlphaTexture{ .landTextureFormID = kBareTexture, .quadrant = 0, .layerIndex = 0 });
	world.land.vertexAlphas.push_back(GameData::LandVertexAlpha{ .quadrant = 0, .layerIndex = 0, .position = 4 * 17 + 4, .opacity = 0.8f });
	world.land.vertexAlphas.push_back(GameData::LandVertexAlpha{ .quadrant = 0, .layerIndex = 0, .position = 8 * 17 + 8, .opacity = 0.3f });
	constexpr FormID kWorld{ 0x3C };
	world.snapshot.landsByWorldspace[kWorld].push_back(world.land);

	const Grass::LandTextureMask mask(world.snapshot, kWorld, { kBareTexture });
	EXPECT_TRUE(mask.Contains(3000.0f, 3000.0f));
	EXPECT_FALSE(mask.Contains(1000.0f, 3000.0f));
	// Nearest vertex (4, 4) is at (512, 512).
	EXPECT_TRUE(mask.Contains(512.0f + 50.0f, 512.0f - 50.0f));
	EXPECT_FALSE(mask.Contains(512.0f + 70.0f, 512.0f));
	EXPECT_FALSE(mask.Contains(1024.0f, 1024.0f));
	// Outside the worldspace's LANDs.
	EXPECT_FALSE(mask.Contains(-100.0f, 100.0f));

	const Grass::LandTextureMask grassy(world.snapshot, kWorld, { kGrassTexture });
	EXPECT_TRUE(grassy.Contains(1000.0f, 1000.0f));
	EXPECT_FALSE(grassy.Contains(512.0f, 512.0f));
}

TEST(BladeEncoding, AppliesTheGlobalScale)
{
	World world;
	world.snapshot.grassesByFormID.at(FormID{ 0x100 }).heightRange = 0.5f;
	auto settings = SmoothSettings();
	const auto plain = Grass::GenerateCellCandidates(world.snapshot, world.land, settings);
	settings.globalScale = 2.0f;
	const auto scaled = Grass::GenerateCellCandidates(world.snapshot, world.land, settings);
	ASSERT_FALSE(plain.blades.empty());
	ASSERT_EQ(plain.blades.size(), scaled.blades.size());
	for (std::size_t i = 0; i < plain.blades.size(); ++i) {
		const auto offset = Grass::Internal::HalfBitsToFloat(plain.blades[i].words[13]);
		// The final scale (1 + offset) doubles.
		EXPECT_NEAR(Grass::Internal::HalfBitsToFloat(scaled.blades[i].words[13]), (1.0f + offset) * 2.0f - 1.0f, 2.0e-3f);
		for (std::size_t w = 0; w < 13; ++w) {
			EXPECT_EQ(plain.blades[i].words[w], scaled.blades[i].words[w]);
		}
	}
}
