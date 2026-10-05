#include "NgioConfig.h"

#include "GameData/LoadOrder.h"

#include "TestSupport.h"

#include <gtest/gtest.h>

#include <vector>

namespace
{
	using namespace FasterNGIO;
	using App::NgioFormRef;

	[[nodiscard]] App::NgioSettings Parse(std::string_view a_text)
	{
		App::NgioSettings settings;
		settings.files.emplace_back("GrassControl.ini");
		App::ParseGrassControlIni(a_text, settings);
		return settings;
	}

	[[nodiscard]] std::vector<GameData::LoadOrderEntry> LoadOrder()
	{
		std::vector<GameData::LoadOrderEntry> entries(3);
		entries[0].pluginName = "skyrim.esm";
		entries[0].fileID = GameData::FileID{ .kind = GameData::ModuleKind::Full, .slot = 0 };
		entries[1].pluginName = "roads.esp";
		entries[1].fileID = GameData::FileID{ .kind = GameData::ModuleKind::Full, .slot = 5 };
		entries[2].pluginName = "small.esl";
		entries[2].kind = GameData::ModuleKind::Light;
		entries[2].fileID = GameData::FileID{ .kind = GameData::ModuleKind::Light, .slot = 3 };
		return entries;
	}
}

TEST(NgioConfig, ReadsTheRayCastAndGrassSettings)
{
	const auto settings = Parse(
		"[RayCastConfig]\n"
		"Ray-cast-enabled = off\n"
		"Ray-cast-height = 200.5\n"
		"Ray-cast-depth = 10\n"
		"Ray-cast-collision-layers = 1 2+13,40 x\n"
		"Ray-cast-width = 12\n"
		"Ray-cast-width-multiplier = 0.5\n"
		"Ray-cast-texture-width = 7.5\n"
		"Grass-cliffs-enabled = false\n"
		"[GrassConfig]\n"
		"Super-dense-grass = yes\n"
		"Super-dense-mode = 9\n"
		"Overwrite-min-grass-size = 30\n"
		"Global-grass-scale = 0.75\n"
		"Skip-pregenerate-world-spaces = A; B ;\n");
	EXPECT_FALSE(settings.rayCast);
	EXPECT_FLOAT_EQ(settings.rayHeight, 200.5f);
	EXPECT_FLOAT_EQ(settings.rayDepth, 10.0f);
	// Layer 40 is beyond the 32-bit mask and 'x' is not a layer; both are skipped.
	EXPECT_EQ(settings.collisionLayerMask, (1u << 1) | (1u << 2) | (1u << 13));
	EXPECT_FLOAT_EQ(settings.rayWidth, 12.0f);
	EXPECT_FLOAT_EQ(settings.rayWidthMultiplier, 0.5f);
	EXPECT_FLOAT_EQ(settings.textureWidth, 7.5f);
	EXPECT_FALSE(settings.grassCliffs);
	EXPECT_TRUE(settings.superDenseGrass);
	EXPECT_EQ(settings.superDenseMode, 9);
	EXPECT_EQ(settings.overwriteMinGrassSize, 30);
	EXPECT_FLOAT_EQ(settings.globalGrassScale, 0.75f);
	EXPECT_EQ(settings.skipWorldspaces, (std::vector<std::string>{ "A", "B" }));
}

TEST(NgioConfig, KeepsNgioDefaultsForMissingKeys)
{
	const auto settings = Parse("[RayCastConfig]\n");
	EXPECT_TRUE(settings.rayCast);
	EXPECT_TRUE(settings.grassCliffs);
	EXPECT_FLOAT_EQ(settings.rayHeight, 150.0f);
	EXPECT_EQ(settings.collisionLayerMask, Collision::kDefaultLayerMask);
	EXPECT_EQ(settings.ensureMaxGrassTypes, -1);
}

TEST(NgioConfig, ReadsRayCastModeAsNgioDoes)
{
	// NGIO stores Ray-cast-mode in Ensure-max-grass-types-setting; the [GrassConfig] key wins.
	auto settings = Parse("[RayCastConfig]\nRay-cast-mode = 0\n");
	EXPECT_EQ(settings.ensureMaxGrassTypes, 0);
	EXPECT_EQ(settings.ignoredRayMode, 0);
	settings = Parse("[RayCastConfig]\nRay-cast-mode = 2\n[GrassConfig]\nEnsure-max-grass-types-setting = -1\n");
	EXPECT_EQ(settings.ensureMaxGrassTypes, -1);
	settings = Parse("[RayCastConfig]\nRay-cast-mode = 1\n");
	EXPECT_FALSE(settings.ignoredRayMode.has_value());
}

TEST(NgioConfig, ParsesFormLists)
{
	const auto forms = App::ParseNgioFormList("1812A:Skyrim.esm; 0x2D5520 : Roads.esp;FE000ABC:Small.esl;bad;12G:Skyrim.esm;12:", "test");
	ASSERT_EQ(forms.size(), 3u);
	EXPECT_EQ(forms[0], (NgioFormRef{ .localID = 0x1812A, .plugin = "skyrim.esm" }));
	EXPECT_EQ(forms[1], (NgioFormRef{ .localID = 0x2D5520, .plugin = "roads.esp" }));
	EXPECT_EQ(forms[2], (NgioFormRef{ .localID = 0x000ABC, .plugin = "small.esl" }));
}

TEST(NgioConfig, ResolvesFormsAgainstTheLoadOrder)
{
	const auto order = LoadOrder();
	EXPECT_EQ(App::ResolveNgioForm({ .localID = 0x1812A, .plugin = "skyrim.esm" }, order), GameData::FormID{ 0x0001812A });
	// The high byte is the plugin's own slot whatever the entry says.
	EXPECT_EQ(App::ResolveNgioForm({ .localID = 0x0A2D5520, .plugin = "roads.esp" }, order), GameData::FormID{ 0x052D5520 });
	// Light plugins keep 12 bits.
	EXPECT_EQ(App::ResolveNgioForm({ .localID = 0x123ABC, .plugin = "small.esl" }, order), GameData::FormID{ 0xFE003ABC });
	EXPECT_FALSE(App::ResolveNgioForm({ .localID = 0x800, .plugin = "missing.esp" }, order).has_value());
}

TEST(NgioConfig, ReadsObjectFiles)
{
	App::NgioSettings settings;
	App::ParseNgioObjectIni(
		"; comment\n"
		"[CliffObjects]\n"
		"13303:Skyrim.esm|Steep|~Rock01|Moss\n"
		"[CliffObjects|Extra]\n"
		"B8D38:Skyrim.esm = anything\n"
		"[IgnoredShapes]\n"
		"800:Roads.esp|RoadEdge|Curb\n"
		"[Other]\n"
		"900:Roads.esp\n",
		settings);
	ASSERT_EQ(settings.cliffObjects.size(), 2u);
	EXPECT_EQ(settings.cliffObjects[0].form, (NgioFormRef{ .localID = 0x13303, .plugin = "skyrim.esm" }));
	EXPECT_TRUE(settings.cliffObjects[0].steep);
	EXPECT_EQ(settings.cliffObjects[0].blockedShapes, (std::vector<std::string>{ "Rock01" }));
	EXPECT_EQ(settings.cliffObjects[0].allowedShapes, (std::vector<std::string>{ "Moss" }));
	EXPECT_FALSE(settings.cliffObjects[1].steep);
	ASSERT_EQ(settings.ignoredShapes.size(), 1u);
	EXPECT_EQ(settings.ignoredShapes[0].shapes, (std::vector<std::string>{ "RoadEdge", "Curb" }));
}

TEST(NgioConfig, ResolvesFeatures)
{
	auto settings = Parse(
		"[RayCastConfig]\n"
		"Ray-cast-ignore-forms = 800:Roads.esp;900:Missing.esp\n"
		"Ray-cast-texture-forms = 801:Roads.esp\n"
		"Ray-cast-ignore-grass-forms = 5:Small.esl\n"
		"Grass-cliffs-forms = 13303:Skyrim.esm\n");
	App::ParseNgioObjectIni("[CliffObjects]\n13303:Skyrim.esm|Steep|Moss\n803:Roads.esp\n[IgnoredShapes]\n800:Roads.esp|Curb\n", settings);
	const auto features = App::ResolveNgioFeatures(settings, LoadOrder());
	EXPECT_EQ(features.ignoredBaseForms, (Rejection::FormSet{ GameData::FormID{ 0x05000800 } }));
	EXPECT_EQ(features.textureForms, (Rejection::FormSet{ GameData::FormID{ 0x05000801 } }));
	EXPECT_EQ(features.ignoredGrassForms, (Rejection::FormSet{ GameData::FormID{ 0xFE003005 } }));
	EXPECT_TRUE(features.cliffs);
	ASSERT_EQ(features.cliffObjects.size(), 2u);
	const auto& cliff = features.cliffObjects.at(GameData::FormID{ 0x00013303 });
	EXPECT_TRUE(cliff.steep);
	EXPECT_EQ(cliff.allowedShapes, (std::vector<std::string>{ "Moss" }));
	EXPECT_TRUE(features.cliffObjects.contains(GameData::FormID{ 0x05000803 }));
	EXPECT_EQ(features.ignoredShapes.at(GameData::FormID{ 0x05000800 }), (std::vector<std::string>{ "Curb" }));

	// Without the file, nothing applies.
	EXPECT_FALSE(App::ResolveNgioFeatures(App::NgioSettings{}, LoadOrder()).cliffs);
}

TEST(NgioConfig, CommandLineValuesWinOverTheFile)
{
	auto settings = Parse(
		"[RayCastConfig]\nRay-cast-enabled = false\nRay-cast-height = 300\nRay-cast-depth = 9\n"
		"[GrassConfig]\nSuper-dense-grass = true\nSuper-dense-mode = 8\nOverwrite-min-grass-size = 40\nEnsure-max-grass-types-setting = 4\n"
		"Global-grass-scale = 2\nOnly-pregenerate-world-spaces = Tamriel\n");

	App::GenerateOptions options;
	options.rejectionOverrides.rayDepth = 2.0f;
	App::ApplyNgioSettings(settings, options);
	EXPECT_FLOAT_EQ(options.rejectionConfig.rayHeight, 300.0f);
	EXPECT_FLOAT_EQ(options.rejectionConfig.rayDepth, 2.0f);
	EXPECT_EQ(options.rejection, App::RejectChoice::None);
	EXPECT_EQ(options.placement.grassPatchSize, options.placement.grassEvalSize << 8);
	EXPECT_EQ(options.minGrassSizeOverride, 40u);
	EXPECT_EQ(options.ensureMaxGrassTypes, 4u);
	EXPECT_FLOAT_EQ(options.placement.globalScale, 2.0f);
	EXPECT_EQ(options.onlyWorldspaces, (std::vector<std::string>{ "Tamriel" }));

	App::GenerateOptions chosen;
	chosen.rejection = App::RejectChoice::Cpu;
	chosen.rejectionChosen = true;
	chosen.minGrassSizeOverride = 25;
	chosen.grassPatchSizeChosen = true;
	chosen.placement.grassPatchSize = 100;
	App::ApplyNgioSettings(settings, chosen);
	EXPECT_EQ(chosen.rejection, App::RejectChoice::Cpu);
	EXPECT_EQ(chosen.minGrassSizeOverride, 25u);
	EXPECT_EQ(chosen.placement.grassPatchSize, 100u);

	// No file: only the command line's values.
	App::GenerateOptions plain;
	plain.rejectionOverrides.rayHeight = 99.0f;
	App::ApplyNgioSettings(App::NgioSettings{}, plain);
	EXPECT_FLOAT_EQ(plain.rejectionConfig.rayHeight, 99.0f);
	EXPECT_EQ(plain.rejection, App::RejectChoice::Auto);
	EXPECT_FLOAT_EQ(plain.placement.globalScale, 1.0f);
}

TEST(NgioConfig, LoadsTheSettingsAndObjectFilesFromData)
{
	const Tests::TempDirectory temp;
	const auto data = temp.Path() / "Data";
	Tests::WriteText(App::DefaultNgioConfigPath(data), "[RayCastConfig]\nRay-cast-height = 120\n");
	Tests::WriteText(data / "Rocks_NGIO.ini", "[CliffObjects]\n13303:Skyrim.esm|Steep\n");
	Tests::WriteText(data / "Unrelated.ini", "[CliffObjects]\n1:Skyrim.esm\n");

	const auto settings = App::LoadNgioSettings(App::DefaultNgioConfigPath(data), data);
	ASSERT_TRUE(settings.Present());
	EXPECT_EQ(settings.files.size(), 2u);
	EXPECT_FLOAT_EQ(settings.rayHeight, 120.0f);
	ASSERT_EQ(settings.cliffObjects.size(), 1u);
	EXPECT_TRUE(settings.cliffObjects[0].steep);

	EXPECT_FALSE(App::LoadNgioSettings(temp.Path() / "missing.ini", data).Present());
}
