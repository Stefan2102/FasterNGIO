#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace FasterNGIO::Collision
{
	struct InstanceShapeCounts
	{
		std::uint32_t triangles{ 0 };
		std::uint32_t vertices{ 0 };
	};

	// The triangle and vertex counts of the shape the engine instances a grass model from: the root
	// node's first child, which BGSGrassManager takes without checking its type. Empty when the NIF
	// does not load or that child is not a BSTriShape. Thread-safe.
	[[nodiscard]] std::optional<InstanceShapeCounts> ReadInstanceShapeCounts(std::span<const std::uint8_t> a_nifBytes);
}
