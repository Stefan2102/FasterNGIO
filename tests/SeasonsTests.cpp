#include "SeasonsConfig.h"

#include "GameData/LoadOrder.h"
#include "GameData/StaticWorld.h"
#include "Seasons/SeasonSwaps.h"

#include <gtest/gtest.h>

#include <string>

namespace
{
	using namespace FasterNGIO;
	using GameData::FormID;
	using GameData::MakeFourCC;
	using Seasons::Season;
	using Seasons::SwapRecord;

	constexpr auto kStat = MakeFourCC('S', 'T', 'A', 'T');
	constexpr auto kTree = MakeFourCC('T', 'R', 'E', 'E');
	constexpr auto kActi = MakeFourCC('A', 'C', 'T', 'I');

	// A snapshot with the material types and land textures the winter rule targets, as Skyrim.esm
	// defines them.
	struct Snapshot
	{
		GameData::StaticWorldSnapshot snapshot;
		std::vector<GameData::LoadOrderEntry> loadOrder;
		std::uint32_t order{ 0 };

		Snapshot()
		{
			auto& skyrim = loadOrder.emplace_back();
			skyrim.pluginName = "skyrim.esm";
			skyrim.path = "Skyrim.esm";
			auto& sos = loadOrder.emplace_back();
			sos.pluginName = "snowoverskyrim.esp";
			sos.path = "SnowOverSkyrim.esp";
			sos.fileID = GameData::FileID{ .kind = GameData::ModuleKind::Full, .slot = 5 };

			Material(0x12F46, "Grass");
			Material(0x12F38, "Dirt");
			Material(0x12F34, "Stone");
			Material(0x12F45, "Snow");
			snapshot.grassesByFormID[FormID{ 0x100 }].formID = FormID{ 0x100 };
			for (const std::uint32_t target : { 0x89Bu, 0x6A1B1u, 0x894u, 0x8B01Eu, 0x6A1AFu, 0xF871Fu, 0x1B082u }) {
				Texture(target, "", 0x12F45, false);
			}
		}

		void Material(std::uint32_t a_form, std::string a_name)
		{
			auto& material = snapshot.materialTypesByFormID[FormID{ a_form }];
			material.formID = FormID{ a_form };
			material.name = std::move(a_name);
		}

		void Created(FormID a_form, const std::string& a_editorID)
		{
			snapshot.creationOrder.try_emplace(a_form, order++);
			if (!a_editorID.empty()) {
				snapshot.firstEditorIDs.try_emplace(a_form, a_editorID);
			}
		}

		void Texture(std::uint32_t a_form, const std::string& a_editorID, std::uint32_t a_material, bool a_grass)
		{
			auto& ltex = snapshot.landTexturesByFormID[FormID{ a_form }];
			ltex.formID = FormID{ a_form };
			ltex.editorID = a_editorID;
			ltex.materialType = FormID{ a_material };
			ltex.grassFormIDs.clear();
			if (a_grass) {
				ltex.grassFormIDs.push_back(FormID{ 0x100 });
			}
			Created(ltex.formID, a_editorID);
		}

		GameData::BaseObjectInfo& Base(std::uint32_t a_form, GameData::FourCC a_signature, std::string a_model, const std::string& a_editorID = {})
		{
			auto& base = snapshot.baseObjectsByFormID[FormID{ a_form }];
			base.formID = FormID{ a_form };
			base.signature = a_signature;
			base.modelPath = std::move(a_model);
			base.editorID = a_editorID;
			Created(base.formID, a_editorID);
			return base;
		}

		void TextureSet(std::uint32_t a_form, std::string a_diffuse)
		{
			auto& set = snapshot.textureSetsByFormID[FormID{ a_form }];
			set.formID = FormID{ a_form };
			set.diffuse = std::move(a_diffuse);
		}
	};

	[[nodiscard]] std::optional<std::uint32_t> Swap(const Seasons::SwapMap& a_map, std::uint32_t a_form)
	{
		const auto it = a_map.find(FormID{ a_form });
		return it != a_map.end() ? std::optional(it->second.value) : std::nullopt;
	}
}

TEST(Seasons, MaterialIdsAreTheCrcOfTheLowerCasedName)
{
	// RE::MATERIAL_ID values, with MNAM as Skyrim.esm writes it.
	EXPECT_EQ(Seasons::MaterialID("Grass"), 1848600814u);
	EXPECT_EQ(Seasons::MaterialID("Dirt"), 3106094762u);
	EXPECT_EQ(Seasons::MaterialID("Stone"), 3741512247u);
	EXPECT_EQ(Seasons::MaterialID("Broken Stone"), 131151687u);
	EXPECT_EQ(Seasons::MaterialID("Gravel"), 428587608u);
	EXPECT_EQ(Seasons::MaterialID("Snow"), 398949039u);
	EXPECT_EQ(Seasons::MaterialID("Ice"), 873356572u);
	EXPECT_EQ(Seasons::MaterialID("Sand"), 2168343821u);
	EXPECT_EQ(Seasons::MaterialID("Mud"), 1486385281u);
}

TEST(Seasons, ReadsPo3SeasonsOfSkyrimIni)
{
	App::SeasonsSettings defaults;
	App::ParseSeasonsIni("", defaults);
	EXPECT_EQ(defaults.seasonType, 5u);
	EXPECT_TRUE(defaults.seasons[0].swapGrass);
	EXPECT_TRUE(defaults.seasons[0].seasonalCaches);
	EXPECT_EQ(defaults.seasons[0].worldspaces.front(), "Tamriel");

	App::SeasonsSettings settings;
	App::ParseSeasonsIni("[Settings]\nSeason Type = 1\n"
						 "[Winter]\nWorldspaces = Tamriel|DLC2SolstheimWorld\nGrass = true\nStatics = false\nSkip Tree = true\n"
						 "[Spring]\nGrass = 1\n"
						 "[Summer]\nGrass = false\n",
		settings);
	EXPECT_EQ(settings.seasonType, 1u);
	const auto& winter = settings.seasons[static_cast<std::size_t>(Season::Winter)];
	EXPECT_EQ(winter.worldspaces, (std::vector<std::string>{ "Tamriel", "DLC2SolstheimWorld" }));
	EXPECT_FALSE(winter.swapObjects[static_cast<std::size_t>(SwapRecord::Statics)]);
	EXPECT_TRUE(winter.swapObjects[static_cast<std::size_t>(SwapRecord::Trees)]);
	EXPECT_TRUE(settings.skipAutomaticWinter[static_cast<std::size_t>(SwapRecord::Trees)]);
	// Seasons reads "1" as true; Grass Cache Helper NG only loads seasonal caches for "true".
	const auto& spring = settings.seasons[static_cast<std::size_t>(Season::Spring)];
	EXPECT_TRUE(spring.swapGrass);
	EXPECT_FALSE(spring.seasonalCaches);
	const auto& summer = settings.seasons[static_cast<std::size_t>(Season::Summer)];
	EXPECT_FALSE(summer.swapGrass);
	EXPECT_FALSE(summer.seasonalCaches);
}

TEST(Seasons, ParsesSwapFiles)
{
	App::SeasonSwapFile file;
	App::ParseSeasonSwapFile("\xEF\xBB\xBF[LandTextures]\n"
							 ";COTN_LRoadDirt02|LDirtSnowPath01\n"
							 "0x5190C~Northern Roads.esp|0x1B082~Skyrim.esm\n"
							 "[trees]\n"
							 "TreeFloraSnowberry02|TreeFloraSnowberry02Snow\n"
							 "= 0x10~Skyrim.esm|0x20~Skyrim.esm\n"
							 "NotAPair\n"
							 "[VisualEffects]\n"
							 "0x1~Skyrim.esm|0x2~Skyrim.esm\n"
							 "[Worldspaces]\nBlackreach\n",
		file);
	const auto& textures = file.entries[static_cast<std::size_t>(SwapRecord::LandTextures)];
	ASSERT_EQ(textures.size(), 1u);
	EXPECT_EQ(textures[0].base, "0x5190C~Northern Roads.esp");
	EXPECT_EQ(textures[0].swap, "0x1B082~Skyrim.esm");
	const auto& trees = file.entries[static_cast<std::size_t>(SwapRecord::Trees)];
	ASSERT_EQ(trees.size(), 2u);
	EXPECT_EQ(trees[0].base, "TreeFloraSnowberry02");
	EXPECT_EQ(trees[1].swap, "0x20~Skyrim.esm");
	EXPECT_EQ(file.worldspaces, (std::vector<std::string>{ "Blackreach" }));
}

TEST(Seasons, GeneratesSnowLandTexturesByMaterial)
{
	Snapshot world;
	world.Texture(0x13428, "LFieldGrass01", 0x12F46, true);   // grass with grass: LGrassSnow01
	world.Texture(0x13429, "LGrassNone", 0x12F46, false);     // grass without: LGrassSnow01NoGrass
	world.Texture(0x1342A, "LFieldGrass02", 0x12F46, true);   // listed: LSnow01
	world.Texture(0x00C16, "LDirt02", 0x12F38, false);        // listed: LSnow02
	world.Texture(0xB424C, "LDirtPath01", 0x12F38, false);    // listed: LDirtSnowPath01
	world.Texture(0x00AE4, "LRocks01", 0x12F34, true);        // stone with grass: LSnowRocks01wGrass
	world.Texture(0x00AE5, "LSnowyField", 0x12F46, true);     // blacklisted by editor ID
	world.Texture(0x00AE6, "LIcePatch", 0x12F45, true);       // snow material: none
	world.Texture(0x00AE7, "LNoMaterial", 0x99999, true);     // no material: LSnow02

	const auto maps = Seasons::GenerateWinterSwaps(world.snapshot, world.loadOrder, true);
	const auto& textures = maps[SwapRecord::LandTextures];
	EXPECT_EQ(Swap(textures, 0x13428), 0x894u);
	EXPECT_EQ(Swap(textures, 0x13429), 0x8B01Eu);
	EXPECT_EQ(Swap(textures, 0x1342A), 0x89Bu);
	EXPECT_EQ(Swap(textures, 0x00C16), 0x6A1B1u);
	EXPECT_EQ(Swap(textures, 0xB424C), 0x1B082u);
	EXPECT_EQ(Swap(textures, 0x00AE4), 0xF871Fu);
	EXPECT_EQ(Swap(textures, 0x00AE5), std::nullopt);
	EXPECT_EQ(Swap(textures, 0x00AE6), std::nullopt);
	EXPECT_EQ(Swap(textures, 0x00AE7), 0x6A1B1u);
	// The snow textures themselves are snow material.
	EXPECT_EQ(Swap(textures, 0x89B), std::nullopt);

	// Without powerofthree's Tweaks, Seasons sees no editor IDs, so the blacklist never matches.
	const auto noTweaks = Seasons::GenerateWinterSwaps(world.snapshot, world.loadOrder, false);
	EXPECT_EQ(Swap(noTweaks[SwapRecord::LandTextures], 0x00AE5), 0x894u);
}

TEST(Seasons, GeneratesSnowObjects)
{
	Snapshot world;
	// SnowOverSkyrim.esp's statics are snow versions of statics with the same model file name.
	world.Base(0x800, kStat, "architecture\\farmhouse\\Farmhouse01.nif", "Farmhouse01");
	world.Base(0x801, kStat, "architecture\\farmhouse\\FarmhouseMoss01.nif", "FarmhouseMoss01");
	world.Base(0x05000800, kStat, "SnowOverSkyrim\\farmhouse01.nif", "Farmhouse01Snow");
	world.Base(0x802, kStat, "architecture\\farmhouse\\Farmhouse01.nif", "IceFarmhouse01");  // blacklisted
	// Trees whose model contains "Snow" replace the trees whose model contains the rest.
	world.Base(0x900, kTree, "landscape\\trees\\TreePineForest01.nif", "TreePineForest01");
	world.Base(0x901, kTree, "landscape\\trees\\TreePineForestSnow01.nif", "TreePineForestSnow01");
	// Activators whose alternate textures are all snowy replace those sharing the model.
	world.TextureSet(0x700, "textures\\landscape\\snow01.dds");
	world.TextureSet(0x701, "textures\\landscape\\dirt01.dds");
	world.Base(0xA00, kActi, "clutter\\Barrel01.nif", "Barrel01");
	world.Base(0xA01, kActi, "clutter\\Barrel01.nif", "Barrel01Snow").alternateTextureSets = { FormID{ 0x700 } };
	world.Base(0xA02, kActi, "clutter\\Barrel01.nif", "Barrel01Dirt").alternateTextureSets = { FormID{ 0x701 } };

	const auto maps = Seasons::GenerateWinterSwaps(world.snapshot, world.loadOrder, true);
	EXPECT_EQ(Swap(maps[SwapRecord::Statics], 0x800), 0x05000800u);
	// "Moss" is removed from the model path before matching.
	EXPECT_EQ(Swap(maps[SwapRecord::Statics], 0x801), 0x05000800u);
	EXPECT_EQ(Swap(maps[SwapRecord::Statics], 0x802), std::nullopt);
	EXPECT_EQ(Swap(maps[SwapRecord::Statics], 0x05000800), std::nullopt);
	EXPECT_EQ(Swap(maps[SwapRecord::Trees], 0x900), 0x901u);
	EXPECT_EQ(Swap(maps[SwapRecord::Activators], 0xA00), 0xA01u);
	EXPECT_EQ(Swap(maps[SwapRecord::Activators], 0xA02), 0xA01u);
	EXPECT_EQ(Swap(maps[SwapRecord::Activators], 0xA01), std::nullopt);
}

TEST(Seasons, PlansOnePassPerDistinctSetOfSwaps)
{
	Snapshot world;
	world.Texture(0x13428, "LFieldGrass01", 0x12F46, true);
	world.Base(0x800, kStat, "rock.nif");
	world.Base(0x801, kStat, "rocksnow.nif");
	world.Base(0x900, kTree, "tree.nif");

	App::ResolvedSeasons seasons;
	for (const auto season : Seasons::kSeasons) {
		auto& entry = seasons.seasons.emplace_back();
		entry.season = season;
	}
	auto& winter = seasons.seasons[0];
	winter.maps[SwapRecord::LandTextures].emplace(FormID{ 0x13428 }, FormID{ 0x894 });
	winter.maps[SwapRecord::Statics].emplace(FormID{ 0x800 }, FormID{ 0x801 });
	// A [Statics] entry only applies to statics, and its replacement must be a base object.
	winter.maps[SwapRecord::Statics].emplace(FormID{ 0x900 }, FormID{ 0x801 });
	winter.maps[SwapRecord::Statics].emplace(FormID{ 0x801 }, FormID{ 0x12345 });
	// Spring swaps a texture for one with the same grass: no different cache.
	world.Texture(0x13429, "LFieldGrass01b", 0x12F46, true);
	seasons.seasons[1].maps[SwapRecord::LandTextures].emplace(FormID{ 0x13428 }, FormID{ 0x13429 });

	const auto passes = App::PlanSeasonPasses(seasons, world.snapshot, "Tamriel", true);
	ASSERT_EQ(passes.size(), 2u);
	EXPECT_EQ(passes[0].suffixes, (std::vector<std::string>{ "", "SPR", "SUM", "AUT" }));
	EXPECT_EQ(passes[1].suffixes, (std::vector<std::string>{ "WIN" }));
	EXPECT_EQ(passes[1].landTextureGrass.size(), 1u);
	ASSERT_EQ(passes[1].baseObjects.size(), 1u);
	EXPECT_EQ(passes[1].baseObjects.at(FormID{ 0x800 }), FormID{ 0x801 });

	// Outside the season's worldspaces nothing applies, so every season copies the plain cache.
	const auto elsewhere = App::PlanSeasonPasses(seasons, world.snapshot, "Blackreach", true);
	ASSERT_EQ(elsewhere.size(), 1u);
	EXPECT_EQ(elsewhere[0].suffixes.size(), 5u);

	// Without rejection, object swaps change nothing.
	const auto grassOnly = App::PlanSeasonPasses(seasons, world.snapshot, "Tamriel", false);
	ASSERT_EQ(grassOnly.size(), 2u);
	EXPECT_TRUE(grassOnly[1].baseObjects.empty());

	// Seasons' "Grass = false": the land textures keep their own grass.
	seasons.seasons[0].config.swapGrass = false;
	seasons.seasons[0].config.swapObjects[static_cast<std::size_t>(SwapRecord::Statics)] = false;
	EXPECT_EQ(App::PlanSeasonPasses(seasons, world.snapshot, "Tamriel", true).size(), 1u);
}

TEST(Seasons, ResolvesSwapFilesLaterEntriesWinning)
{
	Snapshot world;
	world.loadOrder[0].fileID = GameData::FileID{ .kind = GameData::ModuleKind::Full, .slot = 0 };
	auto& light = world.loadOrder.emplace_back();
	light.pluginName = "light.esl";
	light.fileID = GameData::FileID{ .kind = GameData::ModuleKind::Light, .slot = 3 };
	world.Texture(0x13428, "LFieldGrass01", 0x12F46, true);
	world.Texture(0xFE003801, "LLight", 0x12F38, true);

	App::SeasonsSettings settings;
	settings.enabled = true;
	settings.tweaksEditorIDs = true;
	settings.ignoreAutomaticWinter = true;
	App::SeasonSwapFile first;
	first.path = "A_SPR.ini";
	first.season = Season::Spring;
	first.entries[static_cast<std::size_t>(SwapRecord::LandTextures)] = { { "0x13428~Skyrim.esm", "0x89B~Skyrim.esm" }, { "0x801~Light.esl", "LFieldGrass01" } };
	App::SeasonSwapFile second;
	second.path = "B_SPR.ini";
	second.season = Season::Spring;
	second.entries[static_cast<std::size_t>(SwapRecord::LandTextures)] = { { "LFieldGrass01", "0x6A1B1~Skyrim.esm" }, { "0x1~Missing.esp", "0x2~Skyrim.esm" } };
	second.worldspaces = { "Blackreach" };
	settings.swapFiles = { first, second };

	const auto resolved = App::ResolveSeasons(settings, world.snapshot, world.loadOrder);
	ASSERT_EQ(resolved.seasons.size(), 4u);
	const auto& spring = resolved.seasons[1];
	ASSERT_EQ(spring.season, Season::Spring);
	EXPECT_EQ(Swap(spring.maps[SwapRecord::LandTextures], 0x13428), 0x6A1B1u);
	EXPECT_EQ(Swap(spring.maps[SwapRecord::LandTextures], 0xFE003801), 0x13428u);
	EXPECT_EQ(spring.maps[SwapRecord::LandTextures].size(), 2u);
	EXPECT_EQ(spring.config.worldspaces.back(), "Blackreach");
	// Winter ignores the automatic swaps here and has no files.
	EXPECT_TRUE(resolved.seasons[0].maps.Empty());

	// Editor IDs only resolve through powerofthree's Tweaks.
	settings.tweaksEditorIDs = false;
	const auto noTweaks = App::ResolveSeasons(settings, world.snapshot, world.loadOrder);
	EXPECT_EQ(Swap(noTweaks.seasons[1].maps[SwapRecord::LandTextures], 0x13428), 0x89Bu);
}
