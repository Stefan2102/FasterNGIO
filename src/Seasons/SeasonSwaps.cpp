#include "Seasons/SeasonSwaps.h"

#include "GameData/LoadOrder.h"
#include "GameData/StaticWorld.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace FasterNGIO::Seasons
{
	namespace
	{
		using GameData::FormID;
		using GameData::MakeFourCC;

		constexpr auto kSigStat = MakeFourCC('S', 'T', 'A', 'T');
		constexpr auto kSigTree = MakeFourCC('T', 'R', 'E', 'E');
		constexpr auto kSigActi = MakeFourCC('A', 'C', 'T', 'I');
		constexpr auto kSigFurn = MakeFourCC('F', 'U', 'R', 'N');
		constexpr auto kSigMstt = MakeFourCC('M', 'S', 'T', 'T');
		constexpr auto kSigFlor = MakeFourCC('F', 'L', 'O', 'R');
		constexpr auto kSigLtex = MakeFourCC('L', 'T', 'E', 'X');

		// RE::MATERIAL_ID values the land-texture rule switches on.
		constexpr std::uint32_t kMaterialGrass = 1848600814u;
		constexpr std::uint32_t kMaterialDirt = 3106094762u;
		constexpr std::uint32_t kMaterialStone = 3741512247u;
		constexpr std::uint32_t kMaterialStoneBroken = 131151687u;
		constexpr std::uint32_t kMaterialGravel = 428587608u;
		constexpr std::uint32_t kMaterialSnow = 398949039u;
		constexpr std::uint32_t kMaterialIce = 873356572u;
		constexpr std::uint32_t kMaterialSand = 2168343821u;
		constexpr std::uint32_t kMaterialMud = 1486385281u;

		// REX::STR::ICONTAINS: an empty needle never matches.
		[[nodiscard]] bool IContains(std::string_view a_haystack, std::string_view a_needle)
		{
			if (a_needle.empty() || a_needle.size() > a_haystack.size()) {
				return false;
			}
			return !std::ranges::search(a_haystack, a_needle, [](unsigned char a_lhs, unsigned char a_rhs) {
				return std::toupper(a_lhs) == std::toupper(a_rhs);
			}).empty();
		}

		[[nodiscard]] std::string ToLower(std::string_view a_text)
		{
			std::string result(a_text);
			std::ranges::transform(result, result.begin(), [](unsigned char a_ch) { return static_cast<char>(std::tolower(a_ch)); });
			return result;
		}

		// REX::STR::REPLACE_ALL and REPLACE_LAST_INSTANCE: case-sensitive.
		void ReplaceAll(std::string& a_text, std::string_view a_search)
		{
			for (auto pos = a_text.find(a_search); pos != std::string::npos; pos = a_text.find(a_search, pos)) {
				a_text.erase(pos, a_search.size());
			}
		}

		void RemoveLast(std::string& a_text, std::string_view a_search)
		{
			if (const auto pos = a_text.rfind(a_search); pos != std::string::npos) {
				a_text.erase(pos, a_search.size());
			}
		}

		// model::process_model_path: from the last backslash (kept), lower-cased.
		[[nodiscard]] std::string ProcessModelPath(std::string_view a_model)
		{
			if (const auto pos = a_model.rfind('\\'); pos != std::string_view::npos) {
				a_model.remove_prefix(pos);
			}
			return ToLower(a_model);
		}

		class WinterGenerator
		{
		public:
			WinterGenerator(const GameData::StaticWorldSnapshot& a_snapshot, std::span<const GameData::LoadOrderEntry> a_loadOrder, bool a_editorIDs) :
				_snapshot(a_snapshot), _editorIDs(a_editorIDs)
			{
				for (const auto& [formID, base] : a_snapshot.baseObjectsByFormID) {
					_formsBySignature[base.signature].push_back(std::addressof(base));
				}
				for (auto& [signature, forms] : _formsBySignature) {
					std::ranges::sort(forms, {}, [&](const GameData::BaseObjectInfo* a_form) { return CreationOrder(a_form->formID); });
				}
				for (const auto& [formID, ltex] : a_snapshot.landTexturesByFormID) {
					_landTextures.push_back(std::addressof(ltex));
				}
				std::ranges::sort(_landTextures, {}, [&](const GameData::LandTextureInfo* a_ltex) { return CreationOrder(a_ltex->formID); });
				for (const auto& entry : a_loadOrder) {
					if (entry.pluginName == "snowoverskyrim.esp") {
						_snowOverSkyrim = entry.fileID;
					}
				}
				if (a_editorIDs) {
					for (const auto& [formID, material] : a_snapshot.materialObjectsByFormID) {
						if (IContains(EditorID(formID), "Snow")) {
							_snowShaders.push_back(formID);
						}
					}
				}
			}

			[[nodiscard]] SwapMap LandTextures() const
			{
				SwapMap map;
				for (const auto* ltex : _landTextures) {
					if (const auto snow = LandTextureSnowVariant(*ltex)) {
						map.emplace(ltex->formID, *snow);
					}
				}
				return map;
			}

			[[nodiscard]] SwapMap Statics() const
			{
				constexpr std::array<std::string_view, 3> snowBlacklist{ "Ice", "Icicle", "Frozen" };
				constexpr std::array<std::string_view, 7> blacklist{ "Ice", "Icicle", "Frozen", "LoadScreen", "INTERIOR", "INV", "DynDOLOD" };
				const auto inBlacklist = [&](const GameData::BaseObjectInfo& a_stat, std::span<const std::string_view> a_list) {
					const auto editorID = EditorID(a_stat.formID);
					return std::ranges::any_of(a_list, [&](std::string_view a_word) { return IContains(editorID, a_word); });
				};

				const auto& statics = Forms(kSigStat);
				std::map<std::string, const GameData::BaseObjectInfo*> snowStatics;
				if (_snowOverSkyrim) {
					for (const auto* stat : statics) {
						if (IsFormInMod(*_snowOverSkyrim, stat->formID) && !stat->modelPath.empty()) {
							snowStatics.emplace(ProcessModelPath(stat->modelPath), stat);
						}
					}
				}
				for (const auto* stat : statics) {
					const bool snowShader = HasSnowShader(*stat);
					if ((snowShader && OnlyContainsTextureSet(*stat, "Snow", "Mask")) || MustOnlyContainTextureSet(*stat, "Snow", "Mask")) {
						if (stat->modelPath.empty() || inBlacklist(*stat, snowBlacklist)) {
							continue;
						}
						snowStatics.emplace(ProcessModelPath(stat->modelPath), stat);
					}
				}

				SwapMap map;
				for (const auto& [snowPath, snowStat] : snowStatics) {
					for (const auto* stat : statics) {
						std::string path = stat->modelPath;
						RemoveLast(path, "Moss");
						if (IContains(path, snowPath) && snowStat != stat && !HasSnowShader(*stat) && !inBlacklist(*stat, blacklist)) {
							map.emplace(stat->formID, snowStat->formID);
						}
					}
				}
				return map;
			}

			[[nodiscard]] SwapMap Trees() const
			{
				const auto& trees = Forms(kSigTree);
				std::map<std::string, const GameData::BaseObjectInfo*> snowTrees;
				for (const auto* tree : trees) {
					if (std::string path = tree->modelPath; IContains(path, "Snow")) {
						ReplaceAll(path, "Snow");
						snowTrees.emplace(path, tree);
					}
				}
				SwapMap map;
				for (const auto& [path, snowTree] : snowTrees) {
					for (const auto* tree : trees) {
						if (IContains(tree->modelPath, path) && tree != snowTree) {
							map.emplace(tree->formID, snowTree->formID);
						}
					}
				}
				return map;
			}

			// get_snow_variants_by_form: activators, furniture and movable statics.
			[[nodiscard]] SwapMap ByTextureSwap(GameData::FourCC a_signature) const
			{
				constexpr std::array<std::string_view, 3> blacklist{ "Blacksmith", "Frozen", "Marker" };
				const auto& forms = Forms(a_signature);
				std::map<std::string, const GameData::BaseObjectInfo*> snowForms;
				for (const auto* form : forms) {
					if (OnlyContainsTextureSet(*form, "Snow") && !form->modelPath.empty()) {
						snowForms.emplace(ProcessModelPath(form->modelPath), form);
					}
				}
				SwapMap map;
				for (const auto& [path, snowForm] : snowForms) {
					for (const auto* form : forms) {
						if (IContains(form->modelPath, path) && !ContainsTextureSet(*form, "Snow") && !ContainsTextureSet(*form, "Frozen")) {
							if (std::ranges::any_of(blacklist, [&](std::string_view a_word) { return IContains(form->modelPath, a_word); })) {
								continue;
							}
							map.emplace(form->formID, snowForm->formID);
						}
					}
				}
				return map;
			}

		private:
			[[nodiscard]] std::uint32_t CreationOrder(FormID a_form) const
			{
				const auto it = _snapshot.creationOrder.find(a_form);
				return it != _snapshot.creationOrder.end() ? it->second : 0xFFFFFFFFu;
			}

			// clib_util::editorID::get_editorID: for these form types, only powerofthree's Tweaks
			// provides one (the first definition's).
			[[nodiscard]] std::string_view EditorID(FormID a_form) const
			{
				if (!_editorIDs) {
					return {};
				}
				const auto it = _snapshot.firstEditorIDs.find(a_form);
				return it != _snapshot.firstEditorIDs.end() ? std::string_view(it->second) : std::string_view{};
			}

			[[nodiscard]] const std::vector<const GameData::BaseObjectInfo*>& Forms(GameData::FourCC a_signature) const
			{
				static const std::vector<const GameData::BaseObjectInfo*> none;
				const auto it = _formsBySignature.find(a_signature);
				return it != _formsBySignature.end() ? it->second : none;
			}

			// TESFile::IsFormInMod: by form ID, so forms the plugin only overrides do not count.
			[[nodiscard]] static bool IsFormInMod(GameData::FileID a_file, FormID a_form)
			{
				if (a_file.kind == GameData::ModuleKind::Light) {
					return (a_form.value & 0xFFFFF000u) == a_file.BaseFormID();
				}
				return (a_form.value >> 24) == a_file.slot;
			}

			// Cache::is_snow_shader: a material object whose editor ID contains "Snow".
			[[nodiscard]] bool HasSnowShader(const GameData::BaseObjectInfo& a_form) const
			{
				return !a_form.materialObject.IsEmpty() && _snapshot.materialObjectsByFormID.contains(a_form.materialObject) &&
				       std::ranges::find(_snowShaders, a_form.materialObject) != _snowShaders.end();
			}

			// The diffuse path of an alternate texture's set; null when the set does not resolve.
			[[nodiscard]] const std::string* Diffuse(FormID a_textureSet) const
			{
				const auto it = _snapshot.textureSetsByFormID.find(a_textureSet);
				return it != _snapshot.textureSetsByFormID.end() ? std::addressof(it->second.diffuse) : nullptr;
			}

			// model::contains_textureset.
			[[nodiscard]] bool ContainsTextureSet(const GameData::BaseObjectInfo& a_form, std::string_view a_word) const
			{
				return std::ranges::any_of(a_form.alternateTextureSets, [&](FormID a_set) {
					const auto* diffuse = Diffuse(a_set);
					return diffuse && IContains(*diffuse, a_word);
				});
			}

			// model::only_contains_textureset(form, word): false without alternate textures.
			[[nodiscard]] bool OnlyContainsTextureSet(const GameData::BaseObjectInfo& a_form, std::string_view a_word) const
			{
				if (a_form.alternateTextureSets.empty()) {
					return false;
				}
				return std::ranges::all_of(a_form.alternateTextureSets, [&](FormID a_set) {
					const auto* diffuse = Diffuse(a_set);
					return diffuse && IContains(*diffuse, a_word);
				});
			}

			[[nodiscard]] bool AllContainEither(const GameData::BaseObjectInfo& a_form, std::string_view a_first, std::string_view a_second) const
			{
				return std::ranges::all_of(a_form.alternateTextureSets, [&](FormID a_set) {
					const auto* diffuse = Diffuse(a_set);
					return diffuse && (IContains(*diffuse, a_first) || IContains(*diffuse, a_second));
				});
			}

			// model::only_contains_textureset(form, {first, second}): true without alternate textures.
			[[nodiscard]] bool OnlyContainsTextureSet(const GameData::BaseObjectInfo& a_form, std::string_view a_first, std::string_view a_second) const
			{
				return a_form.alternateTextureSets.empty() || AllContainEither(a_form, a_first, a_second);
			}

			// model::must_only_contain_textureset: false without alternate textures.
			[[nodiscard]] bool MustOnlyContainTextureSet(const GameData::BaseObjectInfo& a_form, std::string_view a_first, std::string_view a_second) const
			{
				return !a_form.alternateTextureSets.empty() && AllContainEither(a_form, a_first, a_second);
			}

			[[nodiscard]] bool HasGrass(const GameData::LandTextureInfo& a_ltex) const
			{
				return std::ranges::any_of(a_ltex.grassFormIDs, [&](FormID a_grass) { return _snapshot.grassesByFormID.contains(a_grass); });
			}

			// FormSwapMap::GenerateLandTextureSnowVariant.
			[[nodiscard]] std::optional<FormID> LandTextureSnowVariant(const GameData::LandTextureInfo& a_ltex) const
			{
				constexpr std::array<std::string_view, 6> blacklist{ "Snow", "Ice", "Winter", "Frozen", "Coast", "River" };
				const auto editorID = EditorID(a_ltex.formID);
				if (std::ranges::any_of(blacklist, [&](std::string_view a_word) { return IContains(editorID, a_word); })) {
					return std::nullopt;
				}

				constexpr std::uint32_t LSnow01 = 0x0000089B;
				constexpr std::uint32_t LSnow02 = 0x0006A1B1;
				std::uint32_t material = 0;
				if (const auto it = _snapshot.materialTypesByFormID.find(a_ltex.materialType); it != _snapshot.materialTypesByFormID.end()) {
					material = MaterialID(it->second.name);
				}

				std::uint32_t formID = 0;
				switch (material) {
				case kMaterialGrass:
					switch (a_ltex.formID.value) {
					case 0x0001342A:  // LFieldGrass02
					case 0x00024E46:  // LFieldGrass01NoGrass
					case 0x00024E30:  // LTundra01
					case 0x000A2741:  // LTundra01NoGrass
						formID = LSnow01;
						break;
					case 0x000134B7:  // LFieldDirtGrass01
					case 0x000300E4:  // LTundra02
						formID = LSnow02;
						break;
					default:
						formID = HasGrass(a_ltex) ? 0x00000894u /* LGrassSnow01 */ : 0x0008B01Eu /* LGrassSnow01NoGrass */;
						break;
					}
					break;
				case kMaterialDirt:
					switch (a_ltex.formID.value) {
					case 0x00000C16:  // LDirt02
						formID = LSnow02;
						break;
					case 0x000B424C:  // LDirtPath01
						formID = 0x0001B082u;  // LDirtSnowPath01
						break;
					default:
						formID = LSnow01;
						break;
					}
					break;
				case kMaterialStone:
				case kMaterialStoneBroken:
				case kMaterialGravel:
					switch (a_ltex.formID.value) {
					case 0x0002C6C6:  // LTundraRocks01
						formID = 0x0006A1AFu;  // LSnowRocks01
						break;
					case 0x0006DE8B:  // LTundraRocks01NoRocks
						formID = LSnow01;
						break;
					default:
						formID = HasGrass(a_ltex) ? 0x000F871Fu /* LSnowRockswGrass */ : 0x0006A1AFu /* LSnowRocks01 */;
						break;
					}
					break;
				case kMaterialSnow:
				case kMaterialIce:
				case kMaterialSand:
				case kMaterialMud:
					return std::nullopt;
				default:
					formID = LSnow02;
					break;
				}
				// LookupByID<TESLandTexture>: the target must exist.
				if (!_snapshot.landTexturesByFormID.contains(FormID{ formID })) {
					return std::nullopt;
				}
				return FormID{ formID };
			}

			const GameData::StaticWorldSnapshot& _snapshot;
			bool _editorIDs{ false };
			std::map<GameData::FourCC, std::vector<const GameData::BaseObjectInfo*>> _formsBySignature;
			std::vector<const GameData::LandTextureInfo*> _landTextures;
			std::optional<GameData::FileID> _snowOverSkyrim;
			std::vector<FormID> _snowShaders;
		};
	}

	std::string_view SeasonName(Season a_season)
	{
		constexpr std::array<std::string_view, kSeasonCount> names{ "Winter", "Spring", "Summer", "Autumn" };
		return names[static_cast<std::size_t>(a_season)];
	}

	std::string_view SeasonSuffix(Season a_season)
	{
		constexpr std::array<std::string_view, kSeasonCount> suffixes{ "WIN", "SPR", "SUM", "AUT" };
		return suffixes[static_cast<std::size_t>(a_season)];
	}

	std::string_view SwapRecordSection(SwapRecord a_record)
	{
		constexpr std::array<std::string_view, kSwapRecordCount> sections{ "LandTextures", "Activators", "Furniture", "MovableStatics", "Statics", "Trees",
			"Flora" };
		return sections[static_cast<std::size_t>(a_record)];
	}

	GameData::FourCC SwapRecordSignature(SwapRecord a_record)
	{
		constexpr std::array<GameData::FourCC, kSwapRecordCount> signatures{ kSigLtex, kSigActi, kSigFurn, kSigMstt, kSigStat, kSigTree, kSigFlor };
		return signatures[static_cast<std::size_t>(a_record)];
	}

	bool SwapMaps::Empty() const
	{
		return std::ranges::all_of(records, [](const SwapMap& a_map) { return a_map.empty(); });
	}

	std::uint32_t MaterialID(std::string_view a_name)
	{
		static const auto table = [] {
			std::array<std::uint32_t, 256> result{};
			for (std::uint32_t i = 0; i < 256; ++i) {
				auto value = i;
				for (int bit = 0; bit < 8; ++bit) {
					value = (value & 1u) ? (value >> 1) ^ 0xEDB88320u : value >> 1;
				}
				result[i] = value;
			}
			return result;
		}();
		std::uint32_t crc = 0;
		for (const unsigned char ch : a_name) {
			crc = table[(crc ^ static_cast<unsigned char>(std::tolower(ch))) & 0xFFu] ^ (crc >> 8);
		}
		return crc;
	}

	SwapMaps GenerateWinterSwaps(const GameData::StaticWorldSnapshot& a_snapshot, std::span<const GameData::LoadOrderEntry> a_loadOrder, bool a_editorIDs)
	{
		const WinterGenerator generator(a_snapshot, a_loadOrder, a_editorIDs);
		SwapMaps maps;
		maps[SwapRecord::LandTextures] = generator.LandTextures();
		maps[SwapRecord::Activators] = generator.ByTextureSwap(kSigActi);
		maps[SwapRecord::Furniture] = generator.ByTextureSwap(kSigFurn);
		maps[SwapRecord::MovableStatics] = generator.ByTextureSwap(kSigMstt);
		maps[SwapRecord::Statics] = generator.Statics();
		maps[SwapRecord::Trees] = generator.Trees();
		return maps;
	}
}
