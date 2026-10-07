#pragma once

#include "Options.h"
#include "Rejection/RejectionFeatures.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FasterNGIO::GameData
{
	struct LoadOrderEntry;
}

// NGIO's own settings: Data/SKSE/Plugins/GrassControl.ini and the *_NGIO.ini object files, read as
// NGIO (No Grass In Objects, GrassControl) reads them, so a cache FasterNGIO generates follows the
// same configuration. Only settings that change the cache are kept.
namespace FasterNGIO::App
{
	// One "formid:Plugin.esp" entry: a plugin-local form ID and the plugin's lower-case file name.
	struct NgioFormRef
	{
		std::uint32_t localID{ 0 };
		std::string plugin;

		friend bool operator==(const NgioFormRef&, const NgioFormRef&) = default;
	};

	// A [CliffObjects] line: "formid:Plugin.esp|Shape|~BlockedShape|Steep".
	struct NgioCliffObject
	{
		NgioFormRef form;
		bool steep{ false };
		std::vector<std::string> allowedShapes;
		std::vector<std::string> blockedShapes;
	};

	// An [IgnoredShapes] line: "formid:Plugin.esp|Shape|Shape".
	struct NgioIgnoredShapes
	{
		NgioFormRef form;
		std::vector<std::string> shapes;
	};

	// GrassControl.ini's values, with NGIO's defaults for any key a file leaves out.
	struct NgioSettings
	{
		// The files read; empty when there was no GrassControl.ini (NGIO not installed, or none
		// requested), and then nothing below applies.
		std::vector<std::filesystem::path> files;

		bool rayCast{ true };
		float rayHeight{ 150.0f };
		float rayDepth{ 5.0f };
		// NGIO reads Ray-cast-mode into Ensure-max-grass-types-setting (its Config.cpp), so the mode
		// stays at its default (the capsule) whatever the file says; kept to say so in the log.
		std::optional<std::int32_t> ignoredRayMode;
		float rayWidth{ 0.0f };
		float rayWidthMultiplier{ 0.3f };
		std::uint32_t collisionLayerMask{ Collision::kDefaultLayerMask };
		std::vector<NgioFormRef> ignoreForms;
		std::vector<NgioFormRef> textureForms;
		std::vector<NgioFormRef> ignoreGrassForms;
		float textureWidth{ 5.0f };
		bool grassCliffs{ true };
		std::vector<NgioFormRef> grassCliffForms;

		// What decides whether the game generates grass for a cell without a cache file: NGIO turns
		// the engine's bAllowCreateGrass off only with Use-grass-cache and Only-load-from-cache on and
		// Updating-Cache off (GidFileCache::FixFileFormat).
		bool useGrassCache{ false };
		bool onlyLoadFromCache{ false };
		bool updatingCache{ false };

		bool superDenseGrass{ false };
		std::int32_t superDenseMode{ 8 };
		std::int32_t ensureMaxGrassTypes{ -1 };
		std::int32_t overwriteMinGrassSize{ -1 };
		float globalGrassScale{ 1.0f };
		std::vector<std::string> skipWorldspaces;
		std::vector<std::string> onlyWorldspaces;

		// From the *_NGIO.ini files.
		std::vector<NgioCliffObject> cliffObjects;
		std::vector<NgioIgnoredShapes> ignoredShapes;

		[[nodiscard]] bool Present() const { return !files.empty(); }
		// The game only loads grass from cache files, so a cell without one simply has none.
		[[nodiscard]] bool OnlyLoadsFromCache() const { return Present() && useGrassCache && onlyLoadFromCache && !updatingCache; }
	};

	// Where NGIO keeps its settings, relative to the Data folder.
	[[nodiscard]] std::filesystem::path DefaultNgioConfigPath(const std::filesystem::path& a_data);

	// Reads a_config (GrassControl.ini), then every *_NGIO.ini in a_data. A missing a_config gives
	// settings that are not Present().
	[[nodiscard]] NgioSettings LoadNgioSettings(const std::filesystem::path& a_config, const std::filesystem::path& a_data);

	// The parsers behind LoadNgioSettings, for tests. Problems are logged and the entry skipped.
	void ParseGrassControlIni(std::string_view a_text, NgioSettings& a_settings);
	void ParseNgioObjectIni(std::string_view a_text, NgioSettings& a_settings);
	[[nodiscard]] std::vector<NgioFormRef> ParseNgioFormList(std::string_view a_text, std::string_view a_settingName);

	// The settings a run uses: --ngio-config's file, none, or GrassControl.ini in the Data folder when
	// it exists; logged. Throws when an explicitly named file cannot be read.
	[[nodiscard]] NgioSettings LoadNgioSettingsFor(const GenerateOptions& a_options);

	// One line describing resolved rejection settings.
	void LogRejectionFeatures(const Rejection::RejectionFeatures& a_features);

	// Folds a_settings into a_options where the command line (or the launcher) chose nothing: the
	// ray-cast settings, Ray-cast-enabled, the placement settings, the worldspace lists and whether
	// empty cells get a file. The command line's ray-cast values are applied either way.
	void ApplyNgioSettings(const NgioSettings& a_settings, GenerateOptions& a_options);

	// The load-order form ID of a_ref, or nullopt when its plugin is not loaded.
	[[nodiscard]] std::optional<GameData::FormID> ResolveNgioForm(const NgioFormRef& a_ref, std::span<const GameData::LoadOrderEntry> a_loadOrder);

	// a_settings' lists and objects as rejection inputs; entries whose plugin is not loaded are
	// logged and dropped.
	[[nodiscard]] Rejection::RejectionFeatures ResolveNgioFeatures(const NgioSettings& a_settings, std::span<const GameData::LoadOrderEntry> a_loadOrder);
}
