#include "Collision/InstanceShape.h"

#include <NifFile.hpp>

#include <spanstream>

namespace FasterNGIO::Collision
{
	std::optional<InstanceShapeCounts> ReadInstanceShapeCounts(std::span<const std::uint8_t> a_nifBytes)
	{
		try {
			nifly::NifFile nif;
			std::ispanstream stream(std::span<const char>(reinterpret_cast<const char*>(a_nifBytes.data()), a_nifBytes.size()));
			if (nif.Load(stream) != 0) {
				return std::nullopt;
			}
			const auto* root = nif.GetRootNode();
			if (!root || root->childRefs.GetSize() == 0) {
				return std::nullopt;
			}
			const auto* shape = nif.GetHeader().GetBlock<nifly::BSTriShape>(root->childRefs.GetBlockRef(0));
			if (!shape) {
				return std::nullopt;
			}
			return InstanceShapeCounts{ .triangles = shape->GetNumTriangles(), .vertices = shape->GetNumVertices() };
		} catch (const std::exception&) {
			return std::nullopt;
		}
	}
}
