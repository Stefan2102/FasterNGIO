#include "NgioConfig.h"

#include "GameData/LoadOrder.h"
#include "Platform/IniFile.h"
#include "Platform/Text.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>

namespace FasterNGIO::App
{
	namespace
	{
		// Splits at any of a_separators, trimming each piece and dropping empty ones.
		[[nodiscard]] std::vector<std::string_view> Split(std::string_view a_text, std::string_view a_separators)
		{
			std::vector<std::string_view> parts;
			while (!a_text.empty()) {
				const auto end = a_text.find_first_of(a_separators);
				if (const auto part = Platform::Trim(a_text.substr(0, end)); !part.empty()) {
					parts.push_back(part);
				}
				a_text = end == std::string_view::npos ? std::string_view{} : a_text.substr(end + 1);
			}
			return parts;
		}

		// SimpleIni's GetBoolValue, as NGIO reads its flags: the first letter decides.
		[[nodiscard]] std::optional<bool> ParseIniBool(std::string_view a_value)
		{
			if (a_value.empty()) {
				return std::nullopt;
			}
			switch (a_value[0]) {
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
				if (a_value.size() > 1 && (a_value[1] == 'n' || a_value[1] == 'N')) {
					return true;
				}
				if (a_value.size() > 1 && (a_value[1] == 'f' || a_value[1] == 'F')) {
					return false;
				}
				return std::nullopt;
			default:
				return std::nullopt;
			}
		}

		// SimpleIni's GetLongValue: the leading number, decimal or 0x hex.
		[[nodiscard]] std::optional<std::int32_t> ParseIniLong(const std::string& a_value)
		{
			const char* begin = a_value.c_str();
			char* end = nullptr;
			const bool hex = a_value.size() > 2 && a_value[0] == '0' && (a_value[1] == 'x' || a_value[1] == 'X');
			const auto value = std::strtol(begin, &end, hex ? 16 : 10);
			if (end == begin) {
				return std::nullopt;
			}
			return static_cast<std::int32_t>(value);
		}

		// SimpleIni's GetDoubleValue: the leading number.
		[[nodiscard]] std::optional<float> ParseIniDouble(const std::string& a_value)
		{
			const char* begin = a_value.c_str();
			char* end = nullptr;
			const auto value = std::strtod(begin, &end);
			if (end == begin) {
				return std::nullopt;
			}
			return static_cast<float>(value);
		}

		[[nodiscard]] std::optional<std::string> ReadText(const std::filesystem::path& a_path)
		{
			std::ifstream input(a_path, std::ios::binary);
			if (!input) {
				return std::nullopt;
			}
			return std::string{ std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
		}

		// "formid:Plugin.esp|a|b" -> the form and the rest.
		[[nodiscard]] std::optional<std::pair<NgioFormRef, std::vector<std::string_view>>> ParseObjectLine(std::string_view a_line, std::string_view a_section)
		{
			auto parts = Split(a_line, "|");
			if (parts.empty()) {
				return std::nullopt;
			}
			auto forms = ParseNgioFormList(parts.front(), a_section);
			if (forms.empty()) {
				return std::nullopt;
			}
			parts.erase(parts.begin());
			return std::pair{ std::move(forms.front()), std::move(parts) };
		}

		void AddFormSet(Rejection::FormSet& a_set, const std::vector<NgioFormRef>& a_refs, std::span<const GameData::LoadOrderEntry> a_loadOrder,
			std::string_view a_settingName)
		{
			for (const auto& ref : a_refs) {
				if (const auto form = ResolveNgioForm(ref, a_loadOrder)) {
					a_set.insert(*form);
				} else {
					spdlog::warn("NGIO {}: {:X}:{} is not in the load order", a_settingName, ref.localID, ref.plugin);
				}
			}
		}
	}

	std::filesystem::path DefaultNgioConfigPath(const std::filesystem::path& a_data)
	{
		return a_data / "SKSE" / "Plugins" / "GrassControl.ini";
	}

	std::vector<NgioFormRef> ParseNgioFormList(std::string_view a_text, std::string_view a_settingName)
	{
		std::vector<NgioFormRef> forms;
		for (const auto entry : Split(a_text, ";")) {
			const auto colon = entry.find(':');
			if (colon == std::string_view::npos || colon == 0) {
				spdlog::warn("NGIO {}: expected formid:Plugin.esp, not '{}'", a_settingName, entry);
				continue;
			}
			auto id = Platform::Trim(entry.substr(0, colon));
			const auto plugin = Platform::Trim(entry.substr(colon + 1));
			// As NGIO: an "FE" or "0x" prefix is dropped.
			if (id.size() > 2 && (Platform::IEquals(id.substr(0, 2), "fe") || Platform::IEquals(id.substr(0, 2), "0x"))) {
				id.remove_prefix(2);
			}
			std::uint32_t value = 0;
			const auto result = std::from_chars(id.data(), id.data() + id.size(), value, 16);
			if (id.empty() || result.ec != std::errc{} || result.ptr != id.data() + id.size() || plugin.empty()) {
				spdlog::warn("NGIO {}: expected formid:Plugin.esp, not '{}'", a_settingName, entry);
				continue;
			}
			forms.push_back(NgioFormRef{ .localID = value, .plugin = Platform::LowerAscii(plugin) });
		}
		return forms;
	}

	void ParseGrassControlIni(std::string_view a_text, NgioSettings& a_settings)
	{
		const auto ini = Platform::IniFile::Parse(a_text);
		const auto get = [&](std::string_view a_section, std::string_view a_key) { return ini.Get(a_section, a_key); };
		const auto readBool = [&](std::string_view a_section, std::string_view a_key, bool& a_value) {
			if (const auto text = get(a_section, a_key)) {
				a_value = ParseIniBool(*text).value_or(a_value);
			}
		};
		const auto readInt = [&](std::string_view a_section, std::string_view a_key, std::int32_t& a_value) {
			if (const auto text = get(a_section, a_key)) {
				a_value = ParseIniLong(*text).value_or(a_value);
			}
		};
		const auto readFloat = [&](std::string_view a_section, std::string_view a_key, float& a_value) {
			if (const auto text = get(a_section, a_key)) {
				a_value = ParseIniDouble(*text).value_or(a_value);
			}
		};
		const auto readForms = [&](std::string_view a_key, std::vector<NgioFormRef>& a_value) {
			if (const auto text = get("RayCastConfig", a_key)) {
				a_value = ParseNgioFormList(*text, a_key);
			}
		};
		const auto readNames = [&](std::string_view a_key, std::vector<std::string>& a_value) {
			if (const auto text = get("GrassConfig", a_key)) {
				a_value.clear();
				for (const auto name : Split(*text, ";")) {
					a_value.emplace_back(name);
				}
			}
		};

		readBool("RayCastConfig", "Ray-cast-enabled", a_settings.rayCast);
		readFloat("RayCastConfig", "Ray-cast-height", a_settings.rayHeight);
		readFloat("RayCastConfig", "Ray-cast-depth", a_settings.rayDepth);
		if (const auto layers = get("RayCastConfig", "Ray-cast-collision-layers")) {
			std::uint32_t mask = 0;
			for (const auto layer : Split(*layers, " ,\t+")) {
				std::int32_t value = -1;
				const auto result = std::from_chars(layer.data(), layer.data() + layer.size(), value);
				if (result.ec != std::errc{} || value < 0 || value >= 64) {
					spdlog::warn("NGIO Ray-cast-collision-layers: '{}' is not a layer (0-63)", layer);
				} else if (value >= 32) {
					spdlog::warn("NGIO Ray-cast-collision-layers: layer {} is not supported (only 0-31)", value);
				} else {
					mask |= 1u << value;
				}
			}
			a_settings.collisionLayerMask = mask;
		}
		readForms("Ray-cast-ignore-forms", a_settings.ignoreForms);
		readForms("Ray-cast-texture-forms", a_settings.textureForms);
		readForms("Ray-cast-ignore-grass-forms", a_settings.ignoreGrassForms);
		readFloat("RayCastConfig", "Ray-cast-texture-width", a_settings.textureWidth);
		readBool("RayCastConfig", "Grass-cliffs-enabled", a_settings.grassCliffs);
		readForms("Grass-cliffs-forms", a_settings.grassCliffForms);
		// NGIO's reader stores Ray-cast-mode in Ensure-max-grass-types-setting, which the [GrassConfig]
		// key then overwrites when present; the ray mode itself is never read.
		if (const auto text = get("RayCastConfig", "Ray-cast-mode")) {
			if (const auto mode = ParseIniLong(*text)) {
				a_settings.ensureMaxGrassTypes = *mode;
				if (*mode != 1) {
					a_settings.ignoredRayMode = *mode;
				}
			}
		}
		readFloat("RayCastConfig", "Ray-cast-width", a_settings.rayWidth);
		readFloat("RayCastConfig", "Ray-cast-width-multiplier", a_settings.rayWidthMultiplier);

		readBool("GrassConfig", "Use-grass-cache", a_settings.useGrassCache);
		readBool("GrassConfig", "Only-load-from-cache", a_settings.onlyLoadFromCache);
		readBool("GrassConfig", "Updating-Cache", a_settings.updatingCache);
		readBool("GrassConfig", "Super-dense-grass", a_settings.superDenseGrass);
		readInt("GrassConfig", "Super-dense-mode", a_settings.superDenseMode);
		readInt("GrassConfig", "Ensure-max-grass-types-setting", a_settings.ensureMaxGrassTypes);
		readInt("GrassConfig", "Overwrite-min-grass-size", a_settings.overwriteMinGrassSize);
		readFloat("GrassConfig", "Global-grass-scale", a_settings.globalGrassScale);
		readNames("Skip-pregenerate-world-spaces", a_settings.skipWorldspaces);
		readNames("Only-pregenerate-world-spaces", a_settings.onlyWorldspaces);
	}

	void ParseNgioObjectIni(std::string_view a_text, NgioSettings& a_settings)
	{
		// Each entry is a key (any "= value" is ignored) in a [CliffObjects] or [IgnoredShapes]
		// section, or a "CliffObjects|..." / "IgnoredShapes|..." one.
		std::string section;
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
				section = std::string(Platform::Trim(name.substr(0, name.find('|'))));
				continue;
			}
			const auto key = Platform::Trim(line.substr(0, line.find('=')));
			if (section == "CliffObjects") {
				if (auto parsed = ParseObjectLine(key, "CliffObjects")) {
					NgioCliffObject object;
					object.form = std::move(parsed->first);
					for (const auto part : parsed->second) {
						if (part.find('~') != std::string_view::npos) {
							std::string name(part);
							std::erase(name, '~');
							object.blockedShapes.push_back(std::move(name));
						} else if (part.find("Steep") != std::string_view::npos || part.find("steep") != std::string_view::npos) {
							object.steep = true;
						} else {
							object.allowedShapes.emplace_back(part);
						}
					}
					a_settings.cliffObjects.push_back(std::move(object));
				}
			} else if (section == "IgnoredShapes") {
				if (auto parsed = ParseObjectLine(key, "IgnoredShapes")) {
					NgioIgnoredShapes object;
					object.form = std::move(parsed->first);
					for (const auto part : parsed->second) {
						object.shapes.emplace_back(part);
					}
					a_settings.ignoredShapes.push_back(std::move(object));
				}
			}
		}
	}

	NgioSettings LoadNgioSettings(const std::filesystem::path& a_config, const std::filesystem::path& a_data)
	{
		NgioSettings settings;
		const auto text = ReadText(a_config);
		if (!text) {
			return settings;
		}
		settings.files.push_back(a_config);
		ParseGrassControlIni(*text, settings);

		// As NGIO: every .ini directly in Data whose name has the _NGIO suffix, in name order.
		std::vector<std::filesystem::path> objectFiles;
		std::error_code error;
		for (std::filesystem::directory_iterator it(a_data, error), end; !error && it != end; it.increment(error)) {
			const auto& path = it->path();
			const auto stem = Platform::LowerAscii(Platform::Utf8(path.stem()));
			if (it->is_regular_file(error) && Platform::IEquals(Platform::Utf8(path.extension()), ".ini") && stem.find("_ngio") != std::string::npos) {
				objectFiles.push_back(path);
			}
		}
		std::ranges::sort(objectFiles);
		for (const auto& path : objectFiles) {
			if (const auto objectText = ReadText(path)) {
				settings.files.push_back(path);
				ParseNgioObjectIni(*objectText, settings);
			}
		}
		return settings;
	}

	NgioSettings LoadNgioSettingsFor(const GenerateOptions& a_options)
	{
		if (a_options.ngioConfig && a_options.ngioConfig->empty()) {
			spdlog::info("NGIO settings: not read (--ngio-config none)");
			return {};
		}
		const auto path = a_options.ngioConfig.value_or(DefaultNgioConfigPath(a_options.dataPath));
		auto settings = LoadNgioSettings(path, a_options.dataPath);
		if (!settings.Present()) {
			if (a_options.ngioConfig) {
				throw std::invalid_argument("cannot read the NGIO settings: " + Platform::Utf8(path));
			}
			spdlog::info("NGIO settings: none ({} not found)", Platform::Utf8(path));
			return settings;
		}
		for (const auto& file : settings.files) {
			spdlog::info("NGIO settings: {}", Platform::Utf8(file));
		}
		spdlog::info("NGIO settings: ray cast {}, height {} depth {} width {} x{}; grass cliffs {}; global scale {}{}", settings.rayCast ? "on" : "off",
			settings.rayHeight, settings.rayDepth, settings.rayWidth, settings.rayWidthMultiplier, settings.grassCliffs ? "on" : "off", settings.globalGrassScale,
			settings.superDenseGrass ? std::format("; super-dense mode {}", settings.superDenseMode) : std::string{});
		if (settings.ignoredRayMode) {
			spdlog::warn("NGIO settings: Ray-cast-mode = {} has no effect in NGIO (its reader stores the value as Ensure-max-grass-types-setting), so the "
						 "capsule query is kept; --ray-mode chooses the query here",
				*settings.ignoredRayMode);
		}
		return settings;
	}

	void LogRejectionFeatures(const Rejection::RejectionFeatures& a_features)
	{
		std::size_t steep = 0;
		std::size_t shapeFiltered = 0;
		for (const auto& [form, cliff] : a_features.cliffObjects) {
			steep += cliff.steep ? 1 : 0;
			shapeFiltered += !cliff.allowedShapes.empty() || !cliff.blockedShapes.empty() ? 1 : 0;
		}
		spdlog::info("rejection settings: collision layers {:08X}; ignoring {} base form(s) and {} grass type(s); {} land texture(s) within {}; grass cliffs {} "
					 "({} form(s), {} steep, {} shape-filtered); {} form(s) with ignored shapes",
			a_features.layerMask, a_features.ignoredBaseForms.size(), a_features.ignoredGrassForms.size(), a_features.textureForms.size(), a_features.textureWidth,
			a_features.cliffs ? "on" : "off", a_features.cliffObjects.size(), steep, shapeFiltered, a_features.ignoredShapes.size());
	}

	void ApplyNgioSettings(const NgioSettings& a_settings, GenerateOptions& a_options)
	{
		auto& config = a_options.rejectionConfig;
		if (a_settings.Present()) {
			config.rayHeight = a_settings.rayHeight;
			config.rayDepth = a_settings.rayDepth;
			config.rayWidth = a_settings.rayWidth;
			config.rayWidthMultiplier = a_settings.rayWidthMultiplier;
			if (!a_settings.rayCast && !a_options.rejectionChosen) {
				a_options.rejection = RejectChoice::None;
			}

			auto& placement = a_options.placement;
			if (a_settings.superDenseGrass && !a_options.grassPatchSizeChosen) {
				// NGIO replaces the engine's "<< 7" in the patch size with this shift.
				placement.grassPatchSize = placement.grassEvalSize << std::clamp(a_settings.superDenseMode, 0, 12);
			}
			if (a_settings.overwriteMinGrassSize >= 0 && !a_options.minGrassSizeOverride) {
				a_options.minGrassSizeOverride = static_cast<std::uint32_t>(a_settings.overwriteMinGrassSize);
			}
			if (a_settings.ensureMaxGrassTypes > 0) {
				a_options.ensureMaxGrassTypes = static_cast<std::uint32_t>(a_settings.ensureMaxGrassTypes);
			}
			// As NGIO: ignored unless meaningfully positive.
			if (a_settings.globalGrassScale > 0.0001f) {
				placement.globalScale = a_settings.globalGrassScale;
			}
			a_options.skipWorldspaces = a_settings.skipWorldspaces;
			a_options.onlyWorldspaces = a_settings.onlyWorldspaces;
		}

		if (!a_options.skipEmptyCells) {
			a_options.skipEmptyCells = a_settings.OnlyLoadsFromCache();
		}

		const auto& overrides = a_options.rejectionOverrides;
		config.rayHeight = overrides.rayHeight.value_or(config.rayHeight);
		config.rayDepth = overrides.rayDepth.value_or(config.rayDepth);
		config.mode = overrides.mode.value_or(config.mode);
		config.rayWidth = overrides.rayWidth.value_or(config.rayWidth);
		config.rayWidthMultiplier = overrides.rayWidthMultiplier.value_or(config.rayWidthMultiplier);
	}

	std::optional<GameData::FormID> ResolveNgioForm(const NgioFormRef& a_ref, std::span<const GameData::LoadOrderEntry> a_loadOrder)
	{
		const auto it = std::ranges::find(a_loadOrder, a_ref.plugin, &GameData::LoadOrderEntry::pluginName);
		if (it == a_loadOrder.end()) {
			return std::nullopt;
		}
		const auto objectID = it->fileID.kind == GameData::ModuleKind::Light ? (a_ref.localID & 0xFFFu) : (a_ref.localID & 0xFFFFFFu);
		return GameData::FormID{ it->fileID.BaseFormID() | objectID };
	}

	Rejection::RejectionFeatures ResolveNgioFeatures(const NgioSettings& a_settings, std::span<const GameData::LoadOrderEntry> a_loadOrder)
	{
		Rejection::RejectionFeatures features;
		if (!a_settings.Present()) {
			return features;
		}
		features.layerMask = a_settings.collisionLayerMask;
		AddFormSet(features.ignoredBaseForms, a_settings.ignoreForms, a_loadOrder, "Ray-cast-ignore-forms");
		AddFormSet(features.ignoredGrassForms, a_settings.ignoreGrassForms, a_loadOrder, "Ray-cast-ignore-grass-forms");
		AddFormSet(features.textureForms, a_settings.textureForms, a_loadOrder, "Ray-cast-texture-forms");
		features.textureWidth = a_settings.textureWidth;
		features.cliffs = a_settings.grassCliffs;
		if (features.cliffs) {
			Rejection::FormSet cliffForms;
			AddFormSet(cliffForms, a_settings.grassCliffForms, a_loadOrder, "Grass-cliffs-forms");
			for (const auto form : cliffForms) {
				features.cliffObjects.try_emplace(form);
			}
			// A [CliffObjects] entry adds its shapes and steepness to a listed form, or adds the form.
			for (const auto& object : a_settings.cliffObjects) {
				if (const auto form = ResolveNgioForm(object.form, a_loadOrder)) {
					auto& cliff = features.cliffObjects[*form];
					cliff.steep = cliff.steep || object.steep;
					cliff.allowedShapes.insert(cliff.allowedShapes.end(), object.allowedShapes.begin(), object.allowedShapes.end());
					cliff.blockedShapes.insert(cliff.blockedShapes.end(), object.blockedShapes.begin(), object.blockedShapes.end());
				} else {
					spdlog::warn("NGIO CliffObjects: {:X}:{} is not in the load order", object.form.localID, object.form.plugin);
				}
			}
		}
		for (const auto& object : a_settings.ignoredShapes) {
			if (const auto form = ResolveNgioForm(object.form, a_loadOrder)) {
				auto& shapes = features.ignoredShapes[*form];
				shapes.insert(shapes.end(), object.shapes.begin(), object.shapes.end());
			} else {
				spdlog::warn("NGIO IgnoredShapes: {:X}:{} is not in the load order", object.form.localID, object.form.plugin);
			}
		}
		return features;
	}
}
