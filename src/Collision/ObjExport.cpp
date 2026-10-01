#include "Collision/ObjExport.h"

#include <format>
#include <fstream>
#include <stdexcept>

namespace FasterNGIO::Collision
{
	void WriteObj(const CollisionModel& a_model, const std::filesystem::path& a_path)
	{
		std::ofstream output(a_path);
		if (!output) {
			throw std::runtime_error("failed to open " + a_path.string());
		}
		std::size_t vertexCount = 0;
		const auto writeTriangles = [&](const char* a_group, const std::vector<Triangle>& a_triangles) {
			if (a_triangles.empty()) {
				return;
			}
			output << "g " << a_group << '\n';
			for (const auto& tri : a_triangles) {
				for (const auto& v : tri.vertices) {
					output << std::format("v {} {} {}\n", v.x, v.y, v.z);
				}
				output << std::format("f {} {} {}\n", vertexCount + 1, vertexCount + 2, vertexCount + 3);
				vertexCount += 3;
			}
		};
		writeTriangles("mesh", a_model.triangles);
		writeTriangles("hulls", a_model.hullTriangles);
		if (!a_model.capsules.empty()) {
			output << "g capsules\n";
			for (const auto& capsule : a_model.capsules) {
				output << std::format("v {} {} {}\nv {} {} {}\n", capsule.p0.x, capsule.p0.y, capsule.p0.z, capsule.p1.x, capsule.p1.y, capsule.p1.z);
				output << std::format("l {} {}\n", vertexCount + 1, vertexCount + 2);
				vertexCount += 2;
			}
		}
	}
}
