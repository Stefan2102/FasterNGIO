#include "Archives/ArchiveResolver.h"
#include "GameData/GameData.h"
#include "Platform/DataDirectory.h"

#include "TestSupport.h"

#include <gtest/gtest.h>
#include <zlib.h>

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	using namespace FasterNGIO;
	using GameData::FormID;
	using Tests::TempDirectory;

	constexpr std::uint32_t kCompressed = 1u << 18;
	constexpr std::uint32_t kDeleted = 1u << 5;

	// Builds plugin files record by record, in the on-disk layout.
	class PluginWriter
	{
	public:
		PluginWriter& U8(std::uint8_t a_value) { return Raw(&a_value, 1); }
		PluginWriter& U16(std::uint16_t a_value) { return Raw(&a_value, 2); }
		PluginWriter& U32(std::uint32_t a_value) { return Raw(&a_value, 4); }
		PluginWriter& F32(float a_value) { return Raw(&a_value, 4); }
		PluginWriter& Text(std::string_view a_text) { return Raw(a_text.data(), a_text.size()); }
		PluginWriter& Raw(const void* a_data, std::size_t a_size)
		{
			const auto* bytes = static_cast<const std::uint8_t*>(a_data);
			_bytes.insert(_bytes.end(), bytes, bytes + a_size);
			return *this;
		}
		PluginWriter& Bytes(const std::vector<std::uint8_t>& a_bytes) { return Raw(a_bytes.data(), a_bytes.size()); }

		// A subrecord with a 16-bit size.
		PluginWriter& Sub(std::string_view a_signature, const std::vector<std::uint8_t>& a_data)
		{
			return Text(a_signature).U16(static_cast<std::uint16_t>(a_data.size())).Bytes(a_data);
		}

		// A record header (24 bytes) and its data.
		PluginWriter& Record(std::string_view a_signature, std::uint32_t a_formID, const std::vector<std::uint8_t>& a_data, std::uint32_t a_flags = 0)
		{
			return Text(a_signature).U32(static_cast<std::uint32_t>(a_data.size())).U32(a_flags).U32(a_formID).U32(0).U16(44).U16(0).Bytes(a_data);
		}

		// A group around a_contents: the 24-byte header counts itself.
		PluginWriter& Group(std::uint32_t a_label, std::int32_t a_type, const std::vector<std::uint8_t>& a_contents)
		{
			return Text("GRUP").U32(static_cast<std::uint32_t>(a_contents.size() + 24)).U32(a_label).U32(static_cast<std::uint32_t>(a_type)).U32(0).U32(0).Bytes(a_contents);
		}

		[[nodiscard]] const std::vector<std::uint8_t>& Data() const { return _bytes; }

	private:
		std::vector<std::uint8_t> _bytes;
	};

	[[nodiscard]] std::vector<std::uint8_t> Z(std::string_view a_text)
	{
		std::vector<std::uint8_t> bytes(a_text.begin(), a_text.end());
		bytes.push_back(0);
		return bytes;
	}

	[[nodiscard]] std::uint32_t Label(std::string_view a_signature)
	{
		std::uint32_t value = 0;
		std::memcpy(&value, a_signature.data(), 4);
		return value;
	}

	// GRAS DATA: density, slopes, water distance, ranges, wave period and flags (32 bytes).
	[[nodiscard]] std::vector<std::uint8_t> GrassData(std::uint8_t a_density)
	{
		PluginWriter data;
		data.U8(a_density).U8(0).U8(90).U8(0).U16(0).U16(0).U32(0).F32(32.0f).F32(16.0f).F32(0.5f).F32(12.0f).U8(0x4).U8(0).U16(0);
		return data.Data();
	}

	[[nodiscard]] std::vector<std::uint8_t> Tes4(std::uint32_t a_records, std::initializer_list<std::string_view> a_masters)
	{
		PluginWriter header;
		PluginWriter hedr;
		hedr.F32(1.7f).U32(a_records).U32(0x800);
		header.Sub("HEDR", hedr.Data());
		for (const auto master : a_masters) {
			header.Sub("MAST", Z(master)).Sub("DATA", std::vector<std::uint8_t>(8, 0));
		}
		PluginWriter record;
		record.Record("TES4", 0, header.Data());
		return record.Data();
	}

	// Base.esm: a grass type, a compressed one, a land texture, a static, and Tamriel with one
	// cell holding one reference.
	[[nodiscard]] std::vector<std::uint8_t> BasePlugin()
	{
		PluginWriter grass;
		PluginWriter grassData;
		grassData.Sub("EDID", Z("TestGrass")).Sub("MODL", Z("Grass\\Test.nif")).Sub("DATA", GrassData(40));
		grass.Record("GRAS", 0x000800, grassData.Data());

		// A compressed record: its uncompressed size, then the zlib stream.
		PluginWriter other;
		other.Sub("MODL", Z("Grass\\Other.nif")).Sub("DATA", GrassData(10));
		uLongf compressedSize = compressBound(static_cast<uLong>(other.Data().size()));
		std::vector<std::uint8_t> compressed(compressedSize);
		compress(compressed.data(), &compressedSize, other.Data().data(), static_cast<uLong>(other.Data().size()));
		compressed.resize(compressedSize);
		PluginWriter packed;
		packed.U32(static_cast<std::uint32_t>(other.Data().size())).Bytes(compressed);
		grass.Record("GRAS", 0x000802, packed.Data(), kCompressed);

		PluginWriter ltex;
		PluginWriter gnam;
		gnam.U32(0x000800);
		ltex.Record("LTEX", 0x000801, PluginWriter{}.Sub("GNAM", gnam.Data()).Data());

		PluginWriter stat;
		stat.Record("STAT", 0x000B00, PluginWriter{}.Sub("MODL", Z("Rocks\\Rock.nif")).Data());

		PluginWriter cellData;
		PluginWriter xclc;
		xclc.U32(2).U32(static_cast<std::uint32_t>(-3)).U32(0);
		cellData.Sub("DATA", { 0, 0 }).Sub("XCLC", xclc.Data());
		PluginWriter refData;
		PluginWriter name;
		name.U32(0x000B00);
		PluginWriter position;
		position.F32(9000.0f).F32(-11000.0f).F32(50.0f).F32(0).F32(0).F32(0);
		refData.Sub("NAME", name.Data()).Sub("DATA", position.Data());
		PluginWriter cellChildren;
		cellChildren.Group(0x000900, 9, PluginWriter{}.Record("REFR", 0x000A00, refData.Data()).Data());
		PluginWriter worldChildren;
		worldChildren.Record("CELL", 0x000900, cellData.Data()).Bytes(cellChildren.Data());
		PluginWriter world;
		world.Record("WRLD", 0x00003C, PluginWriter{}.Sub("EDID", Z("Tamriel")).Data()).Group(0x00003C, 1, worldChildren.Data());

		PluginWriter plugin;
		plugin.Bytes(Tes4(7, {}))
			.Group(Label("GRAS"), 0, grass.Data())
			.Group(Label("LTEX"), 0, ltex.Data())
			.Group(Label("STAT"), 0, stat.Data())
			.Group(Label("WRLD"), 0, world.Data());
		return plugin.Data();
	}

	// Patch.esp: overrides the grass, deletes the land texture, and adds a grass type whose MODL
	// is behind an XXXX subrecord.
	[[nodiscard]] std::vector<std::uint8_t> PatchPlugin()
	{
		PluginWriter grass;
		grass.Record("GRAS", 0x000800, PluginWriter{}.Sub("MODL", Z("Grass\\Patched.nif")).Sub("DATA", GrassData(80)).Data());
		const auto model = Z("Grass\\Big.nif");
		PluginWriter xxxx;
		PluginWriter size;
		size.U32(static_cast<std::uint32_t>(model.size()));
		xxxx.Sub("XXXX", size.Data()).Text("MODL").U16(0).Bytes(model).Sub("DATA", GrassData(5));
		grass.Record("GRAS", 0x01000803, xxxx.Data());

		PluginWriter ltex;
		ltex.Record("LTEX", 0x000801, {}, kDeleted);

		PluginWriter plugin;
		plugin.Bytes(Tes4(3, { "Base.esm" })).Group(Label("GRAS"), 0, grass.Data()).Group(Label("LTEX"), 0, ltex.Data());
		return plugin.Data();
	}

	void WriteBytes(const std::filesystem::path& a_path, const std::vector<std::uint8_t>& a_bytes)
	{
		Tests::WriteText(a_path, std::string_view(reinterpret_cast<const char*>(a_bytes.data()), a_bytes.size()));
	}

	struct Install
	{
		TempDirectory temp;
		std::filesystem::path data = temp.Path() / "Data";
		std::filesystem::path pluginsTxt = temp.Path() / "plugins.txt";

		Install()
		{
			WriteBytes(data / "Base.esm", BasePlugin());
			WriteBytes(data / "PATCH.ESP", PatchPlugin());
			Tests::WriteText(pluginsTxt, "# load order\r\n*Base.esm\r\n*patch.esp\r\nDisabled.esp\r\n*Missing.esp\r\n");
		}

		[[nodiscard]] std::vector<GameData::LoadOrderEntry> LoadOrder() const { return GameData::PrepareLoadOrder(GameData::ReadPluginsTxt(data, pluginsTxt)); }
	};

	// A version 104 archive with one uncompressed file.
	[[nodiscard]] std::vector<std::uint8_t> MakeArchive(std::string_view a_folder, std::string_view a_file, std::string_view a_contents)
	{
		const std::uint32_t folderNameLength = static_cast<std::uint32_t>(a_folder.size() + 1);
		const std::uint32_t fileNameLength = static_cast<std::uint32_t>(a_file.size() + 1);
		constexpr std::uint32_t kHeader = 36;
		constexpr std::uint32_t kFolderRecord = 16;
		const std::uint32_t dataOffset = kHeader + kFolderRecord + 1 + folderNameLength + 16 + fileNameLength;
		PluginWriter archive;
		archive.Text(std::string_view("BSA\0", 4)).U32(104).U32(kHeader).U32(0x3).U32(1).U32(1).U32(folderNameLength).U32(fileNameLength).U32(0);
		archive.U32(0).U32(0).U32(1).U32(kHeader + kFolderRecord);
		archive.U8(static_cast<std::uint8_t>(folderNameLength)).Text(a_folder).U8(0);
		archive.U32(0).U32(0).U32(static_cast<std::uint32_t>(a_contents.size())).U32(dataOffset);
		archive.Text(a_file).U8(0);
		archive.Text(a_contents);
		return archive.Data();
	}
}

TEST(GameData, ReadsTheLoadOrderFromPluginsTxt)
{
	const Install install;
	const auto loadOrder = install.LoadOrder();
	// Enabled plugins present in Data, matched case-insensitively; masters in each header.
	ASSERT_EQ(loadOrder.size(), 2u);
	EXPECT_EQ(loadOrder[0].pluginName, "base.esm");
	EXPECT_EQ(loadOrder[1].pluginName, "patch.esp");
	EXPECT_EQ(loadOrder[0].fileID.slot, 0u);
	EXPECT_EQ(loadOrder[1].fileID.slot, 1u);
	EXPECT_EQ(loadOrder[1].masters, std::vector<std::string>{ "base.esm" });
	EXPECT_EQ(loadOrder[0].recordCount, 7u);
}

TEST(GameData, ResolvesOverridesDeletionsAndCompressedRecords)
{
	const Install install;
	const auto loadOrder = install.LoadOrder();
	std::vector<GameData::StaticPluginShard> shards;
	for (const auto& entry : loadOrder) {
		shards.push_back(GameData::ParseStaticWorldShard(entry, loadOrder));
	}
	const auto snapshot = GameData::BuildStaticWorldSnapshot(shards);

	// The patch's version wins.
	ASSERT_TRUE(snapshot.grassesByFormID.contains(FormID{ 0x000800 }));
	const auto& grass = snapshot.grassesByFormID.at(FormID{ 0x000800 });
	EXPECT_EQ(grass.modelPath, "Grass\\Patched.nif");
	EXPECT_EQ(grass.density, 80u);
	EXPECT_FLOAT_EQ(grass.positionRange, 32.0f);
	EXPECT_FLOAT_EQ(grass.wavePeriod, 12.0f);
	EXPECT_TRUE(grass.FitsToSlope());
	// A compressed record, and a subrecord whose size comes from XXXX.
	ASSERT_TRUE(snapshot.grassesByFormID.contains(FormID{ 0x000802 }));
	EXPECT_EQ(snapshot.grassesByFormID.at(FormID{ 0x000802 }).modelPath, "Grass\\Other.nif");
	ASSERT_TRUE(snapshot.grassesByFormID.contains(FormID{ 0x01000803 }));
	EXPECT_EQ(snapshot.grassesByFormID.at(FormID{ 0x01000803 }).modelPath, "Grass\\Big.nif");
	// Deleted by the patch.
	EXPECT_FALSE(snapshot.landTexturesByFormID.contains(FormID{ 0x000801 }));

	ASSERT_TRUE(snapshot.worldsByFormID.contains(FormID{ 0x3C }));
	EXPECT_EQ(snapshot.worldsByFormID.at(FormID{ 0x3C }).editorID, "Tamriel");
	ASSERT_TRUE(snapshot.cellsByFormID.contains(FormID{ 0x900 }));
	const auto& cell = snapshot.cellsByFormID.at(FormID{ 0x900 });
	EXPECT_EQ(cell.worldFormID, FormID{ 0x3C });
	EXPECT_EQ(cell.gridX, 2);
	EXPECT_EQ(cell.gridY, -3);

	const GameData::CellKey key{ FormID{ 0x3C }, 2, -3 };
	ASSERT_TRUE(snapshot.exteriorPlacementsByCell.contains(key));
	const auto& placements = snapshot.exteriorPlacementsByCell.at(key);
	ASSERT_EQ(placements.size(), 1u);
	EXPECT_EQ(placements[0].baseFormID, FormID{ 0xB00 });
	EXPECT_FLOAT_EQ(placements[0].position[0], 9000.0f);
}

TEST(GameData, NormalizesModelPaths)
{
	EXPECT_EQ(GameData::NormalizeModelPath("  Rocks/Rock01.NIF "), "meshes\\rocks\\rock01.nif");
	EXPECT_EQ(GameData::NormalizeModelPath("Meshes\\Trees\\Pine.nif"), "meshes\\trees\\pine.nif");
	EXPECT_EQ(GameData::NormalizeModelPath("textures\\rock.dds"), "");
}

TEST(Archives, ReadsArchivesAndPrefersLooseFiles)
{
	const Install install;
	WriteBytes(install.data / "Base.bsa", MakeArchive("meshes\\test", "box.nif", "archived"));
	WriteBytes(install.data / "Patch.bsa", MakeArchive("Meshes\\Test", "Lid.NIF", "lid"));
	const auto loadOrder = install.LoadOrder();
	{
		const Archives::ArchiveResolver resolver(install.data, Archives::DefaultArchiveOrder(loadOrder));
		const auto box = resolver.Read("Meshes/Test/Box.nif");
		ASSERT_TRUE(box.has_value());
		EXPECT_EQ(std::string(box->begin(), box->end()), "archived");
		const auto lid = resolver.Read("data\\meshes\\test\\lid.nif");
		ASSERT_TRUE(lid.has_value());
		EXPECT_EQ(std::string(lid->begin(), lid->end()), "lid");
		EXPECT_FALSE(resolver.Read("meshes\\test\\none.nif").has_value());
	}

	Tests::WriteText(install.data / "meshes" / "test" / "box.nif", "loose");
	const Archives::ArchiveResolver resolver(install.data, Archives::DefaultArchiveOrder(loadOrder));
	const auto box = resolver.Read("meshes\\test\\box.nif");
	ASSERT_TRUE(box.has_value());
	EXPECT_EQ(std::string(box->begin(), box->end()), "loose");
}

TEST(Archives, LoadsTheArchivesTheIniListsName)
{
	const Install install;
	WriteBytes(install.data / "Custom Grass.bsa", MakeArchive("meshes\\grass", "tuft.nif", "tuft"));
	const auto loadOrder = install.LoadOrder();
	EXPECT_FALSE(Archives::ArchiveResolver(install.data, Archives::DefaultArchiveOrder(loadOrder)).Read("meshes\\grass\\tuft.nif").has_value());

	Archives::ArchiveIniLists ini;
	ini.resourceArchiveList2 = std::vector<std::string>{ "Skyrim - Textures0.bsa", "Custom Grass.bsa" };
	const auto order = Archives::DefaultArchiveOrder(loadOrder, ini);
	// The default first list, the INI's second list, then the plugins' own archives.
	EXPECT_EQ(order[0], "Skyrim - Misc.bsa");
	EXPECT_EQ(order[7], "Skyrim - Textures0.bsa");
	EXPECT_EQ(order[8], "Custom Grass.bsa");
	EXPECT_GT(std::ranges::find(order, "Base.bsa") - order.begin(), 8);
	EXPECT_TRUE(Archives::ArchiveResolver(install.data, order).Read("meshes\\grass\\tuft.nif").has_value());
}

TEST(DataDirectory, FindsNamesCaseInsensitively)
{
	const TempDirectory temp;
	Tests::WriteText(temp.Path() / "Skyrim.ESM");
	const auto found = Platform::FindInDirectory(temp.Path(), "skyrim.esm");
	ASSERT_TRUE(found.has_value());
	EXPECT_TRUE(std::filesystem::exists(*found));
	EXPECT_FALSE(Platform::FindInDirectory(temp.Path(), "update.esm").has_value());
}
