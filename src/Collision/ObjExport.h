#pragma once

#include "Collision/CollisionModel.h"

#include <filesystem>

namespace FasterNGIO::Collision
{
	// Writes a collision model as Wavefront OBJ for inspection: one group per primitive kind.
	// Capsules are written as their axis segments.
	void WriteObj(const CollisionModel& a_model, const std::filesystem::path& a_path);
}
