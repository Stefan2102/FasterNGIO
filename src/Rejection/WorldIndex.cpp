#include "Rejection/WorldIndex.h"

#include "Collision/NifCollisionExtractor.h"

#include <oneapi/tbb/parallel_for.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace FasterNGIO::Rejection
{
	Similarity Similarity::Inverse() const
	{
		// The linear part is rotation * scale, so its inverse is rotation^T / scale.
		Similarity result;
		const auto invScaleSq = 1.0f / (scale * scale);
		for (int row = 0; row < 3; ++row) {
			for (int col = 0; col < 3; ++col) {
				result.m[row][col] = m[col][row] * invScaleSq;
			}
		}
		for (int row = 0; row < 3; ++row) {
			result.m[row][3] = -(result.m[row][0] * m[0][3] + result.m[row][1] * m[1][3] + result.m[row][2] * m[2][3]);
		}
		result.scale = 1.0f / scale;
		return result;
	}

	Similarity Similarity::FromPlacement(const float (&a_position)[3], const float (&a_rotation)[3], float a_scale)
	{
		// Skyrim's reference rotation, as SARP's renderer places statics (its row-vector matrix,
		// transposed into column-vector form).
		const float sx = std::sin(a_rotation[0]), cx = std::cos(a_rotation[0]);
		const float sy = std::sin(a_rotation[1]), cy = std::cos(a_rotation[1]);
		const float sz = std::sin(a_rotation[2]), cz = std::cos(a_rotation[2]);
		const float s = a_scale;
		Similarity result;
		result.m[0][0] = cy * cz * s;
		result.m[0][1] = cy * sz * s;
		result.m[0][2] = -sy * s;
		result.m[1][0] = (sx * sy * cz - cx * sz) * s;
		result.m[1][1] = (sx * sy * sz + cx * cz) * s;
		result.m[1][2] = sx * cy * s;
		result.m[2][0] = (cx * sy * cz + sx * sz) * s;
		result.m[2][1] = (cx * sy * sz - sx * cz) * s;
		result.m[2][2] = cx * cy * s;
		result.m[0][3] = a_position[0];
		result.m[1][3] = a_position[1];
		result.m[2][3] = a_position[2];
		result.scale = s;
		return result;
	}

	WorldIndex::WorldIndex(
		const GameData::StaticWorldSnapshot& a_snapshot,
		GameData::FormID a_worldFormID,
		const Archives::ArchiveResolver& a_resolver,
		const RejectionConfig& a_config,
		float a_maxQueryReach)
	{
		struct PendingReference
		{
			const GameData::PlacementInfo* placement{ nullptr };
			std::uint32_t model{ 0 };
		};

		std::unordered_map<std::string, std::uint32_t> modelIndexByPath;
		std::vector<PendingReference> references;
		for (const auto& [key, placements] : a_snapshot.exteriorPlacementsByCell) {
			if (key.worldFormID != a_worldFormID) {
				continue;
			}
			for (const auto& placement : placements) {
				const auto baseIt = a_snapshot.baseObjectsByFormID.find(placement.baseFormID);
				if (baseIt == a_snapshot.baseObjectsByFormID.end() || baseIt->second.modelPath.empty()) {
					continue;
				}
				++_stats.references;
				if (a_config.ignoreForms.contains(placement.baseFormID)) {
					++_stats.referencesIgnored;
					continue;
				}
				auto path = GameData::NormalizeModelPath(baseIt->second.modelPath);
				const auto [it, inserted] = modelIndexByPath.try_emplace(path, static_cast<std::uint32_t>(_models.size()));
				if (inserted) {
					_models.push_back(ModelRecord{ .path = std::move(path) });
				}
				references.push_back(PendingReference{ .placement = std::addressof(placement), .model = it->second });
			}
		}
		_stats.models = _models.size();

		const auto extractBegin = std::chrono::steady_clock::now();
		const Collision::ExtractionOptions options{ .layerMask = a_config.layerMask };
		std::vector<std::uint8_t> missing(_models.size(), 0);
		oneapi::tbb::parallel_for(std::size_t{ 0 }, _models.size(), [&](std::size_t i) {
			const auto bytes = a_resolver.Read(_models[i].path);
			if (!bytes) {
				missing[i] = 1;
				_models[i].collision.status = Collision::ExtractionStatus::LoadFailed;
				return;
			}
			_models[i].collision = Collision::ExtractCollision(*bytes, options);
		});
		_stats.extractSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - extractBegin).count();
		for (std::size_t i = 0; i < _models.size(); ++i) {
			_stats.modelsMissing += missing[i];
			_stats.modelsWithCollision += _models[i].collision.status == Collision::ExtractionStatus::HasCollision;
		}

		constexpr float cellSize = GameData::kSkyrimTerrainCellSize;
		for (const auto& reference : references) {
			const auto& model = _models[reference.model].collision;
			if (model.status != Collision::ExtractionStatus::HasCollision) {
				continue;
			}
			const auto& placement = *reference.placement;
			Instance instance;
			instance.model = reference.model;
			instance.referenceFormID = placement.formID;
			instance.baseFormID = placement.baseFormID;
			instance.worldFromModel = Similarity::FromPlacement(placement.position, placement.rotation, placement.scale);
			instance.modelFromWorld = instance.worldFromModel.Inverse();

			constexpr auto inf = (std::numeric_limits<float>::max)();
			Float3 lo{ inf, inf, inf };
			Float3 hi{ -inf, -inf, -inf };
			for (int corner = 0; corner < 8; ++corner) {
				const Float3 local{
					(corner & 1) ? model.aabbMax.x : model.aabbMin.x,
					(corner & 2) ? model.aabbMax.y : model.aabbMin.y,
					(corner & 4) ? model.aabbMax.z : model.aabbMin.z,
				};
				const auto world = instance.worldFromModel.Apply(local);
				lo = { (std::min)(lo.x, world.x), (std::min)(lo.y, world.y), (std::min)(lo.z, world.z) };
				hi = { (std::max)(hi.x, world.x), (std::max)(hi.y, world.y), (std::max)(hi.z, world.z) };
			}
			instance.aabbMin = lo;
			instance.aabbMax = hi;

			const auto index = static_cast<std::uint32_t>(_instances.size());
			_instances.push_back(instance);
			++_stats.referencesWithCollision;

			const auto firstX = static_cast<std::int32_t>(std::floor((lo.x - a_maxQueryReach) / cellSize));
			const auto lastX = static_cast<std::int32_t>(std::floor((hi.x + a_maxQueryReach) / cellSize));
			const auto firstY = static_cast<std::int32_t>(std::floor((lo.y - a_maxQueryReach) / cellSize));
			const auto lastY = static_cast<std::int32_t>(std::floor((hi.y + a_maxQueryReach) / cellSize));
			for (auto y = firstY; y <= lastY; ++y) {
				for (auto x = firstX; x <= lastX; ++x) {
					_cells[CellKey(x, y)].push_back(index);
				}
			}
		}
	}

	std::span<const std::uint32_t> WorldIndex::InstancesInCell(std::int32_t a_cellX, std::int32_t a_cellY) const
	{
		const auto it = _cells.find(CellKey(a_cellX, a_cellY));
		return it != _cells.end() ? std::span<const std::uint32_t>(it->second) : std::span<const std::uint32_t>();
	}
}
