#include "Rejection/RejectionConfig.h"

#include "GameData/Records.h"

#include <algorithm>
#include <cmath>

namespace FasterNGIO::Rejection
{
	QueryShape MakeQueryShape(const RejectionConfig& a_config, const GameData::GrassInfo& a_grass)
	{
		QueryShape shape;
		shape.depth = a_config.rayDepth;
		shape.height = a_config.rayHeight;
		if (a_config.mode == QueryMode::Ray) {
			return shape;
		}

		// NGIO sizes the phantom from the GRAS bounds only when both X bounds are non-zero.
		float radius = 20.0f;
		const auto& bounds = a_grass.bounds;
		if (bounds.present && bounds.max[0] != 0 && bounds.min[0] != 0) {
			const auto widthX = std::abs(static_cast<float>(bounds.max[0] - bounds.min[0]) * 0.5f);
			const auto widthY = std::abs(static_cast<float>(bounds.max[1] - bounds.min[1]) * 0.5f);
			radius = (std::max)(widthX, widthY) * 0.5f;
		}
		radius *= a_config.rayWidthMultiplier;
		if (a_config.rayWidth > 0.0f) {
			radius = a_config.rayWidth * 0.5f;
		}
		shape.radius = radius;
		return shape;
	}
}
