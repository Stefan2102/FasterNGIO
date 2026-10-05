#include "SeasonsConfig.h"

#include "GameData/LoadOrder.h"
#include "GameData/StaticWorld.h"
#include "Platform/DataDirectory.h"
#include "Platform/IniFile.h"
#include "Platform/Text.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>

namespace FasterNGIO::App
{
	namespace
	{
		using GameData::FormID;
		using Seasons::Season;
		using Seasons::SwapRecord;

		[[nodiscard]] std::optional<std::string> ReadText(const std::filesystem::path& a_path)
		{
			std::ifstream input(a_path, std::ios::binary);
			if (!input) {
				return std::nullopt;
			}
			return std::string{ std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
		}

		// A file under Data, each component matched case-insensitively.
		[[nodiscard]] std::optional<std::filesystem::path> FindDataPath(const std::filesystem::path& a_data, std::initializer_list<std::string_view> a_components)
		{
			std::filesystem::path current = a_data;
			for (const auto component : a_components) {
				auto next = Platform::FindInDirectory(current, component);
				if (!next) {
					return std::nullopt;
				}
				current = std::move(*next);
			}
			return current;
		}

		// SimpleIni's GetBoolValue: the first letter decides; anything else keeps the default.
		[[nodiscard]] bool IniBool(const Platform::IniFile& a_ini, std::string_view a_section, std::string_view a_key, bool a_default)
		{
			const auto value = a_ini.Get(a_section, a_key);
			if (!value || value->empty()) {
				return a_default;
			}
			switch ((*value)[0]) {
			case 't':
			case 'T':
			case 'y':
			case 'Y':
			case '1':
				return true;
			case 'f':
			case 'F':
			case 'n':
			case 'N':
			case '0':
				return false;
			case 'o':
			case 'O':
				if (value->size() > 1 && ((*value)[1] == 'n' || (*value)[1] == 'N')) {
					return true;
				}
				if (value->size() > 1 && ((*value)[1] == 'f' || (*value)[1] == 'F')) {
					return false;
				}
				return a_default;
			default:
				return a_default;
			}
		}

		[[nodiscard]] std::vector<std::string> SplitWorldspaces(std::string_view a_text)
		{
			std::vector<std::string> names;
			while (!a_text.empty()) {
				const auto end = a_text.find('|');
				if (const auto name = Platform::Trim(a_text.substr(0, end)); !name.empty()) {
					names.emplace_back(name);
				}
				a_text = end == std::string_view::npos ? std::string_view{} : a_text.substr(end + 1);
			}
			return names;
		}

		[[nodiscard]] std::size_t Index(SwapRecord a_record) { return static_cast<std::size_t>(a_record); }

		constexpr std::array kSwapRecords{ SwapRecord::LandTextures, SwapRecord::Activators, SwapRecord::Furniture, SwapRecord::MovableStatics,
			SwapRecord::Statics, SwapRecord::Trees, SwapRecord::Flora };

		// INI::parse_form: "0xID~Plugin.esp" (the ID in hex, plugin-local) or an editor ID, which
		// resolves only when Tweaks loads editor IDs for these form types.
		class FormResolver
		{
		public:
			FormResolver(const GameData::StaticWorldSnapshot& a_snapshot, std::span<const GameData::LoadOrderEntry> a_loadOrder, bool a_editorIDs) :
				_loadOrder(a_loadOrder)
			{
				if (!a_editorIDs) {
					return;
				}
				std::vector<std::pair<std::uint32_t, FormID>> ordered;
				ordered.reserve(a_snapshot.firstEditorIDs.size());
				for (const auto& [form, editorID] : a_snapshot.firstEditorIDs) {
					const auto order = a_snapshot.creationOrder.find(form);
					ordered.emplace_back(order != a_snapshot.creationOrder.end() ? order->second : 0xFFFFFFFFu, form);
				}
				std::ranges::sort(ordered);
				for (const auto& [order, form] : ordered) {
					_byEditorID.try_emplace(Platform::LowerAscii(a_snapshot.firstEditorIDs.at(form)), form);
				}
			}

			[[nodiscard]] std::optional<FormID> Resolve(std::string_view a_text) const
			{
				const auto text = Platform::Trim(a_text);
				if (const auto tilde = text.find('~'); tilde != std::string_view::npos) {
					if (text.find('~', tilde + 1) != std::string_view::npos) {
						return std::nullopt;
					}
					const std::string id(Platform::Trim(text.substr(0, tilde)));
					const auto plugin = Platform::LowerAscii(Platform::Trim(text.substr(tilde + 1)));
					char* end = nullptr;
					const auto local = static_cast<std::uint32_t>(std::strtoul(id.c_str(), &end, 16));
					if (end == id.c_str()) {
						return std::nullopt;
					}
					const auto it = std::ranges::find(_loadOrder, plugin, &GameData::LoadOrderEntry::pluginName);
					if (it == _loadOrder.end()) {
						return std::nullopt;
					}
					const auto objectID = it->fileID.kind == GameData::ModuleKind::Light ? (local & 0xFFFu) : (local & 0xFFFFFFu);
					return FormID{ it->fileID.BaseFormID() | objectID };
				}
				if (const auto it = _byEditorID.find(Platform::LowerAscii(text)); it != _byEditorID.end()) {
					return it->second;
				}
				return std::nullopt;
			}

		private:
			std::span<const GameData::LoadOrderEntry> _loadOrder;
			std::unordered_map<std::string, FormID> _byEditorID;
		};

		// The plugin that defines a form (by its ID) and its plugin-local ID, as Seasons writes them.
		[[nodiscard]] std::string FormText(FormID a_form, std::span<const GameData::LoadOrderEntry> a_loadOrder)
		{
			for (const auto& entry : a_loadOrder) {
				const bool light = entry.fileID.kind == GameData::ModuleKind::Light;
				const bool owns = light ? (a_form.value & 0xFFFFF000u) == entry.fileID.BaseFormID() : (a_form.value >> 24) == entry.fileID.slot && (a_form.value >> 24) != 0xFE;
				if (owns) {
					return std::format("0x{:X}~{}", a_form.value & (light ? 0xFFFu : 0xFFFFFFu), Platform::Utf8(entry.path.filename()));
				}
			}
			return std::format("0x{:X}~?", a_form.value);
		}

		[[nodiscard]] std::string_view EditorIDOf(const GameData::StaticWorldSnapshot& a_snapshot, FormID a_form)
		{
			const auto it = a_snapshot.firstEditorIDs.find(a_form);
			return it != a_snapshot.firstEditorIDs.end() ? std::string_view(it->second) : std::string_view{};
		}

		// Resolves a swap file's entries into a_maps, later entries winning; the entries that did not resolve.
		std::size_t ApplySwapFile(const SeasonSwapFile& a_file, const FormResolver& a_resolver, Seasons::SwapMaps& a_maps)
		{
			std::size_t unresolved = 0;
			for (const auto record : kSwapRecords) {
				for (const auto& entry : a_file.entries[Index(record)]) {
					const auto base = a_resolver.Resolve(entry.base);
					const auto swap = a_resolver.Resolve(entry.swap);
					if (base && swap) {
						a_maps[record].insert_or_assign(*base, *swap);
					} else {
						++unresolved;
						spdlog::debug("Seasons {}: [{}] {}|{} does not resolve", Platform::Utf8(a_file.path.filename()), Seasons::SwapRecordSection(record), entry.base,
							entry.swap);
					}
				}
			}
			return unresolved;
		}

		[[nodiscard]] bool SameGrass(const GameData::StaticWorldSnapshot& a_snapshot, FormID a_lhs, FormID a_rhs)
		{
			return a_snapshot.landTexturesByFormID.at(a_lhs).grassFormIDs == a_snapshot.landTexturesByFormID.at(a_rhs).grassFormIDs;
		}
	}

	void ParseSeasonsIni(std::string_view a_text, SeasonsSettings& a_settings)
	{
		const auto ini = Platform::IniFile::Parse(a_text);
		if (const auto type = ini.Get("Settings", "Season Type")) {
			char* end = nullptr;
			const auto value = std::strtol(type->c_str(), &end, 10);
			if (end != type->c_str()) {
				a_settings.seasonType = static_cast<std::uint32_t>(value);
			}
		}
		for (const auto season : Seasons::kSeasons) {
			const auto section = Seasons::SeasonName(season);
			auto& config = a_settings.seasons[static_cast<std::size_t>(season)];
			if (const auto worldspaces = ini.Get(section, "Worldspaces")) {
				config.worldspaces = SplitWorldspaces(*worldspaces);
			}
			config.swapGrass = IniBool(ini, section, "Grass", true);
			if (const auto grass = ini.Get(section, "Grass")) {
				config.seasonalCaches = Platform::Trim(*grass) == "true";
			}
			config.swapObjects[Index(SwapRecord::Activators)] = IniBool(ini, section, "Activators", true);
			config.swapObjects[Index(SwapRecord::Furniture)] = IniBool(ini, section, "Furniture", true);
			config.swapObjects[Index(SwapRecord::MovableStatics)] = IniBool(ini, section, "Movable Statics", true);
			config.swapObjects[Index(SwapRecord::Statics)] = IniBool(ini, section, "Statics", true);
			config.swapObjects[Index(SwapRecord::Trees)] = IniBool(ini, section, "Trees", true);
			config.swapObjects[Index(SwapRecord::Flora)] = IniBool(ini, section, "Flora", true);
		}
		a_settings.ignoreAutomaticWinter = IniBool(ini, "Winter", "Ignore auto generated WIN formswap", false);
		a_settings.skipAutomaticWinter[Index(SwapRecord::LandTextures)] = IniBool(ini, "Winter", "Skip Land Textures", false);
		a_settings.skipAutomaticWinter[Index(SwapRecord::Activators)] = IniBool(ini, "Winter", "Skip Activator", false);
		a_settings.skipAutomaticWinter[Index(SwapRecord::Furniture)] = IniBool(ini, "Winter", "Skip Furniture", false);
		a_settings.skipAutomaticWinter[Index(SwapRecord::MovableStatics)] = IniBool(ini, "Winter", "Skip Movable Statics", false);
		a_settings.skipAutomaticWinter[Index(SwapRecord::Statics)] = IniBool(ini, "Winter", "Skip Statics", false);
		a_settings.skipAutomaticWinter[Index(SwapRecord::Trees)] = IniBool(ini, "Winter", "Skip Tree", false);
	}

	void ParseSeasonSwapFile(std::string_view a_text, SeasonSwapFile& a_file)
	{
		// Keys only, one "base|swap" per line, as Seasons' SimpleIni (AllowKeyOnly, MultiKey) reads them.
		std::optional<std::size_t> record;
		bool worldspaces = false;
		while (!a_text.empty()) {
			const auto end = a_text.find('\n');
			auto line = Platform::Trim(a_text.substr(0, end));
			a_text = end == std::string_view::npos ? std::string_view{} : a_text.substr(end + 1);
			if (line.starts_with("\xEF\xBB\xBF")) {
				line = Platform::Trim(line.substr(3));
			}
			if (line.empty() || line.front() == ';' || line.front() == '#') {
				continue;
			}
			if (line.front() == '[') {
				const auto close = line.find(']');
				const auto name = Platform::Trim(line.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1));
				record.reset();
				worldspaces = Platform::IEquals(name, "Worldspaces");
				for (const auto candidate : kSwapRecords) {
					if (Platform::IEquals(name, Seasons::SwapRecordSection(candidate))) {
						record = Index(candidate);
					}
				}
				continue;
			}
			const auto equals = line.find('=');
			auto key = Platform::Trim(line.substr(0, equals));
			if (key.empty() && equals != std::string_view::npos) {
				key = Platform::Trim(line.substr(equals + 1));
			}
			if (key.empty()) {
				continue;
			}
			if (worldspaces) {
				a_file.worldspaces.emplace_back(key);
			} else if (record) {
				const auto bar = key.find('|');
				if (bar == std::string_view::npos) {
					continue;
				}
				const auto rest = key.substr(bar + 1);
				a_file.entries[*record].push_back(SeasonSwapEntry{ .base = std::string(key.substr(0, bar)), .swap = std::string(rest.substr(0, rest.find('|'))) });
			}
		}
	}

	SeasonsSettings ReadSeasonsSettings(const std::filesystem::path& a_data, SeasonsChoice a_choice)
	{
		SeasonsSettings settings;
		if (a_choice == SeasonsChoice::Off) {
			return settings;
		}
		const auto& data = a_data;
		settings.installed = FindDataPath(data, { "SKSE", "Plugins", "po3_SeasonsOfSkyrim.dll" }).has_value();
		settings.cacheHelperInstalled = FindDataPath(data, { "SKSE", "Plugins", "GrassCacheHelperNG.dll" }).has_value();
		if (FindDataPath(data, { "SKSE", "Plugins", "po3_Tweaks.dll" })) {
			settings.tweaksEditorIDs = true;
			if (const auto tweaksIni = FindDataPath(data, { "SKSE", "Plugins", "po3_Tweaks.ini" })) {
				if (const auto ini = Platform::IniFile::Load(*tweaksIni)) {
					settings.tweaksEditorIDs = IniBool(*ini, "Fixes", "Load EditorIDs", true);
				}
			}
		}
		if (!settings.installed && a_choice == SeasonsChoice::Auto) {
			return settings;
		}

		const auto plugins = FindDataPath(data, { "SKSE", "Plugins" });
		settings.settingsFile = (plugins ? *plugins : data / "SKSE" / "Plugins") / "po3_SeasonsOfSkyrim.ini";
		if (const auto found = FindDataPath(data, { "SKSE", "Plugins", "po3_SeasonsOfSkyrim.ini" })) {
			settings.settingsFile = *found;
		}
		if (const auto text = ReadText(settings.settingsFile)) {
			settings.settingsFound = true;
			ParseSeasonsIni(*text, settings);
		}
		if (settings.seasonType == 0 && a_choice == SeasonsChoice::Auto) {
			return settings;
		}
		settings.enabled = true;

		if (const auto folder = Platform::FindInDirectory(data, "Seasons")) {
			std::error_code error;
			for (std::filesystem::directory_iterator it(*folder, error), end; !error && it != end; it.increment(error)) {
				if (!it->is_regular_file(error) || Platform::Utf8(it->path().extension()) != ".ini") {
					continue;
				}
				const auto name = Platform::Utf8(it->path().filename());
				const auto stem = Platform::Utf8(it->path().stem());
				if (name.find("MainFormSwap") != std::string::npos) {
					if (stem == "MainFormSwap_WIN") {
						settings.automaticWinterFile = it->path();
					}
					continue;
				}
				for (const auto season : Seasons::kSeasons) {
					if (stem.ends_with(Seasons::SeasonSuffix(season))) {
						auto& file = settings.swapFiles.emplace_back();
						file.path = it->path();
						file.season = season;
					}
				}
			}
			// Seasons sorts each season's files by path.
			std::ranges::sort(settings.swapFiles, {}, [](const SeasonSwapFile& a_file) { return Platform::Utf8(a_file.path.filename()); });
			for (auto& file : settings.swapFiles) {
				if (const auto text = ReadText(file.path)) {
					ParseSeasonSwapFile(*text, file);
				}
			}
		}

		return settings;
	}

	SeasonsSettings LoadSeasonsSettingsFor(const GenerateOptions& a_options)
	{
		auto settings = ReadSeasonsSettings(a_options.dataPath, a_options.seasons);
		if (a_options.seasons == SeasonsChoice::Off) {
			spdlog::info("Seasons of Skyrim: off (--seasons off)");
			return settings;
		}
		if (!settings.enabled) {
			if (settings.installed) {
				spdlog::info("Seasons of Skyrim: disabled (Season Type = 0 in {}), so no seasonal caches", Platform::Utf8(settings.settingsFile));
			} else {
				spdlog::info("Seasons of Skyrim: not installed (no Data/SKSE/Plugins/po3_SeasonsOfSkyrim.dll)");
			}
			return settings;
		}
		spdlog::info("Seasons of Skyrim: {}{}; settings {}; {} swap file(s); editor IDs {}", settings.installed ? "installed" : "not installed (--seasons on)",
			settings.seasonType == 0 ? " but disabled (Season Type = 0)" : "",
			settings.settingsFound ? Platform::Utf8(settings.settingsFile) : std::string("defaults (po3_SeasonsOfSkyrim.ini not found)"), settings.swapFiles.size(),
			settings.tweaksEditorIDs ? "from powerofthree's Tweaks" : "unavailable (no powerofthree's Tweaks), so editor-ID entries and blacklists do not apply");
		for (const auto& file : settings.swapFiles) {
			spdlog::info("Seasons swaps ({}): {}", Seasons::SeasonSuffix(file.season), Platform::Utf8(file.path));
		}
		if (!settings.cacheHelperInstalled) {
			spdlog::warn("Seasons of Skyrim: Grass Cache Helper NG is not installed; the game only loads the seasonal caches through it");
		}
		return settings;
	}

	ResolvedSeasons ResolveSeasons(const SeasonsSettings& a_settings, const GameData::StaticWorldSnapshot& a_snapshot,
		std::span<const GameData::LoadOrderEntry> a_loadOrder)
	{
		ResolvedSeasons resolved;
		if (!a_settings.enabled) {
			return resolved;
		}
		const FormResolver resolver(a_snapshot, a_loadOrder, a_settings.tweaksEditorIDs);
		resolved.automaticWinter = Seasons::GenerateWinterSwaps(a_snapshot, a_loadOrder, a_settings.tweaksEditorIDs);

		for (const auto season : Seasons::kSeasons) {
			const auto& config = a_settings.seasons[static_cast<std::size_t>(season)];
			if (!config.seasonalCaches) {
				spdlog::info("Seasons {}: Grass is not \"true\", so Grass Cache Helper NG loads the plain caches; none written", Seasons::SeasonSuffix(season));
				continue;
			}
			ResolvedSeason entry;
			entry.season = season;
			entry.config = config;
			if (season == Season::Winter && !a_settings.ignoreAutomaticWinter) {
				for (const auto record : kSwapRecords) {
					if (!a_settings.skipAutomaticWinter[Index(record)]) {
						entry.maps[record] = resolved.automaticWinter[record];
					}
				}
			}
			std::size_t unresolved = 0;
			for (const auto& file : a_settings.swapFiles) {
				if (file.season == season) {
					unresolved += ApplySwapFile(file, resolver, entry.maps);
					entry.config.worldspaces.insert(entry.config.worldspaces.end(), file.worldspaces.begin(), file.worldspaces.end());
				}
			}
			std::string counts;
			for (const auto record : kSwapRecords) {
				if (!entry.maps[record].empty()) {
					counts += std::format("{}{} {}", counts.empty() ? "" : ", ", entry.maps[record].size(), Seasons::SwapRecordSection(record));
				}
			}
			spdlog::info("Seasons {}: {}{}", Seasons::SeasonSuffix(season), counts.empty() ? "no swaps" : counts,
				unresolved ? std::format("; {} entr(ies) do not resolve", unresolved) : std::string{});
			resolved.seasons.push_back(std::move(entry));
		}
		return resolved;
	}

	std::vector<SeasonPass> PlanSeasonPasses(const ResolvedSeasons& a_seasons, const GameData::StaticWorldSnapshot& a_snapshot, std::string_view a_worldEditorID,
		bool a_objectSwaps)
	{
		std::vector<SeasonPass> passes(1);
		passes.front().suffixes.emplace_back();
		for (const auto& season : a_seasons.seasons) {
			const auto suffix = std::string(Seasons::SeasonSuffix(season.season));
			const bool inWorld = std::ranges::any_of(season.config.worldspaces, [&](const std::string& a_name) { return Platform::IEquals(a_name, a_worldEditorID); });
			SeasonPass pass;
			pass.suffixes.push_back(suffix);
			if (inWorld && season.config.swapGrass) {
				for (const auto& [from, to] : season.maps[SwapRecord::LandTextures]) {
					// GetSwapLandTexture: a target that is no land texture keeps the original's grass.
					if (a_snapshot.landTexturesByFormID.contains(from) && a_snapshot.landTexturesByFormID.contains(to) && !SameGrass(a_snapshot, from, to)) {
						pass.landTextureGrass.emplace(from, to);
					}
				}
			}
			if (inWorld && a_objectSwaps) {
				for (const auto record : kSwapRecords) {
					if (record == SwapRecord::LandTextures || !season.config.swapObjects[Index(record)]) {
						continue;
					}
					for (const auto& [from, to] : season.maps[record]) {
						// The map is chosen by the original base's type; the replacement must be a base object.
						const auto base = a_snapshot.baseObjectsByFormID.find(from);
						if (base != a_snapshot.baseObjectsByFormID.end() && base->second.signature == Seasons::SwapRecordSignature(record) && from != to &&
							a_snapshot.baseObjectsByFormID.contains(to)) {
							pass.baseObjects.emplace(from, to);
						}
					}
				}
			}
			if (pass.landTextureGrass.empty() && pass.baseObjects.empty()) {
				passes.front().suffixes.push_back(suffix);
				continue;
			}
			const auto same = std::ranges::find_if(passes, [&](const SeasonPass& a_existing) {
				return a_existing.landTextureGrass == pass.landTextureGrass && a_existing.baseObjects == pass.baseObjects;
			});
			if (same != passes.end()) {
				same->suffixes.push_back(suffix);
			} else {
				passes.push_back(std::move(pass));
			}
		}
		return passes;
	}

	void DumpSeasonSwaps(const ResolvedSeasons& a_seasons, const GameData::StaticWorldSnapshot& a_snapshot, std::span<const GameData::LoadOrderEntry> a_loadOrder,
		const std::filesystem::path& a_path)
	{
		std::string text;
		const auto dump = [&](std::string_view a_title, const Seasons::SwapMaps& a_maps) {
			text += std::format("; ===== {} =====\n", a_title);
			for (const auto record : kSwapRecords) {
				if (a_maps[record].empty()) {
					continue;
				}
				text += std::format("[{}]\n", Seasons::SwapRecordSection(record));
				for (const auto& [from, to] : a_maps[record]) {
					text += std::format(";{}|{}\n{}|{}\n", EditorIDOf(a_snapshot, from), EditorIDOf(a_snapshot, to), FormText(from, a_loadOrder), FormText(to, a_loadOrder));
				}
				text += "\n";
			}
		};
		dump("automatic winter swaps (MainFormSwap_WIN.ini)", a_seasons.automaticWinter);
		for (const auto& season : a_seasons.seasons) {
			dump(std::format("{} (automatic and swap files)", Seasons::SeasonName(season.season)), season.maps);
		}
		std::ofstream output(a_path, std::ios::binary);
		output << text;
		if (!output) {
			throw std::runtime_error("cannot write " + Platform::Utf8(a_path));
		}
		spdlog::info("wrote the season swaps to {}", Platform::Utf8(a_path));
	}

	std::size_t CompareAutomaticWinterSwaps(const SeasonsSettings& a_settings, const ResolvedSeasons& a_seasons,
		const GameData::StaticWorldSnapshot& a_snapshot, std::span<const GameData::LoadOrderEntry> a_loadOrder)
	{
		if (a_settings.automaticWinterFile.empty()) {
			return 0;
		}
		const auto text = ReadText(a_settings.automaticWinterFile);
		if (!text) {
			return 0;
		}
		SeasonSwapFile file;
		file.path = a_settings.automaticWinterFile;
		ParseSeasonSwapFile(*text, file);
		const FormResolver resolver(a_snapshot, a_loadOrder, a_settings.tweaksEditorIDs);
		Seasons::SwapMaps seasons;
		(void)ApplySwapFile(file, resolver, seasons);

		std::size_t differences = 0;
		for (const auto record : kSwapRecords) {
			const auto& ours = a_seasons.automaticWinter[record];
			const auto& theirs = seasons[record];
			std::size_t missing = 0;
			std::size_t extra = 0;
			std::size_t changed = 0;
			const auto example = [&](std::string_view a_kind, FormID a_form) {
				if (missing + extra + changed <= 5) {
					spdlog::info("  {} [{}] {} ({})", a_kind, Seasons::SwapRecordSection(record), FormText(a_form, a_loadOrder), EditorIDOf(a_snapshot, a_form));
				}
			};
			for (const auto& [from, to] : theirs) {
				const auto it = ours.find(from);
				if (it == ours.end()) {
					++missing;
					example("only in Seasons':", from);
				} else if (it->second != to) {
					++changed;
					example("different target:", from);
				}
			}
			for (const auto& [from, to] : ours) {
				if (!theirs.contains(from)) {
					++extra;
					example("only generated:", from);
				}
			}
			if (missing + extra + changed != 0 || !ours.empty() || !theirs.empty()) {
				spdlog::info("Seasons automatic winter [{}]: {} generated, {} in {}; {} only there, {} only generated, {} different", Seasons::SwapRecordSection(record),
					ours.size(), theirs.size(), Platform::Utf8(a_settings.automaticWinterFile.filename()), missing, extra, changed);
			}
			differences += missing + extra + changed;
		}
		if (differences != 0) {
			spdlog::warn("Seasons automatic winter swaps differ from {} in {} entr(ies); it is regenerated when the game starts with a changed load order",
				Platform::Utf8(a_settings.automaticWinterFile), differences);
		}
		return differences;
	}

	std::string DescribeSeasons(const SeasonsSettings& a_settings)
	{
		if (!a_settings.enabled) {
			return a_settings.installed ? "Seasons of Skyrim: installed, but no seasonal caches (Season Type = 0)" : "Seasons of Skyrim: not installed";
		}
		std::string seasons;
		for (const auto season : Seasons::kSeasons) {
			if (a_settings.seasons[static_cast<std::size_t>(season)].seasonalCaches) {
				seasons += std::format("{}{}", seasons.empty() ? "" : ", ", Seasons::SeasonSuffix(season));
			}
		}
		return std::format("Seasons of Skyrim: seasonal caches ({}){}", seasons.empty() ? "none" : seasons,
			a_settings.cacheHelperInstalled ? "" : "; Grass Cache Helper NG not found");
	}
}
