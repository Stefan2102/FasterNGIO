#include "Rejection/RejectionConfig.h"

#include <algorithm>
#include <cmath>

namespace FasterNGIO::Rejection
{
	QueryShape MakeQueryShape(const RejectionConfig& a_config, const GameData::GrassInfo& a_grass)
	{
		QueryShape shape;
		shape.test = a_config.enabled && !a_config.ignoreGrassForms.contains(a_grass.formID);
		shape.depth = a_config.rayDepth;
		shape.height = a_config.rayHeight;

		// NGIO sizes the phantom from the GRAS bounds only when both X bounds are non-zero.
		float radius = 20.0f;
		float widthX = 20.0f;
		float widthY = 20.0f;
		const auto& bounds = a_grass.bounds;
		if (bounds.present && bounds.max[0] != 0 && bounds.min[0] != 0) {
			widthX = std::abs(static_cast<float>(bounds.max[0] - bounds.min[0]) * 0.5f);
			widthY = std::abs(static_cast<float>(bounds.max[1] - bounds.min[1]) * 0.5f);
			radius = (std::max)(widthX, widthY) * 0.5f;
		}
		radius *= a_config.rayWidthMultiplier;
		if (a_config.rayWidth > 0.0f) {
			widthX = a_config.rayWidth * 0.5f;
			widthY = a_config.rayWidth * 0.5f;
			radius = a_config.rayWidth * 0.5f;
		}

		switch (a_config.mode) {
		case QueryMode::Ray:
			break;
		case QueryMode::Capsule:
			shape.radius = radius;
			break;
		case QueryMode::Box:
			shape.halfExtentX = widthX;
			shape.halfExtentY = widthY;
			break;
		}
		return shape;
	}
}
