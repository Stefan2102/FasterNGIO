#pragma once

#include "Collision/NifCollisionExtractor.h"
#include "GameData/FormID.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace FasterNGIO::Rejection
{
	using FormSet = std::unordered_set<GameData::FormID, GameData::FormIDHash>;

	// A [CliffObjects] entry of an *_NGIO.ini: a cliff form whose grass placement can be limited to
	// (or kept off) named render shapes, and whose neighbour check allows a larger height difference.
	struct CliffObject
	{
		bool steep{ false };
		std::vector<std::string> allowedShapes;
		std::vector<std::string> blockedShapes;
	};

	// NGIO's [RayCastConfig] lists and *_NGIO.ini objects, resolved to load-order form IDs. Empty:
	// plain rejection, as without NGIO's settings.
	struct RejectionFeatures
	{
		// Ray-cast-collision-layers: collision in other layers does not reject.
		std::uint32_t layerMask{ Collision::kDefaultLayerMask };
		// Ray-cast-ignore-forms: references of these base forms never reject.
		FormSet ignoredBaseForms;
		// Ray-cast-ignore-grass-forms: grass of these types is never rejected.
		FormSet ignoredGrassForms;
		// Ray-cast-texture-forms: grass on (or within textureWidth of) these land textures is rejected.
		FormSet textureForms;
		float textureWidth{ 5.0f };
		// Grass-cliffs-enabled, with Grass-cliffs-forms and [CliffObjects] merged into cliffObjects.
		bool cliffs{ false };
		std::unordered_map<GameData::FormID, CliffObject, GameData::FormIDHash> cliffObjects;
		// [IgnoredShapes]: hits on these forms do not reject grass whose nearest render shape is named.
		std::unordered_map<GameData::FormID, std::vector<std::string>, GameData::FormIDHash> ignoredShapes;
		// Experimental (not an NGIO setting): objects with rejecting collision reject by their render
		// geometry instead (Collision::ExtractionOptions::renderGeometry).
		bool renderGeometry{ false };
	};
}
