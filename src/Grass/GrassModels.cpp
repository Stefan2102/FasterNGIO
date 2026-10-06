#include "Grass/GrassModels.h"

#include "Archives/ArchiveResolver.h"
#include "Collision/InstanceShape.h"
#include "GameData/ModelPath.h"
#include "Grass/CellCache.h"

#include <oneapi/tbb/parallel_for.h>

#include <algorithm>
#include <optional>

namespace FasterNGIO::Grass
{
	GrassModelLayout MeasureGrassModels(const GameData::StaticWorldSnapshot& a_snapshot, const Archives::ArchiveResolver& a_resolver)
	{
		std::unordered_map<std::string, std::size_t> modelIndexByPath;
		std::vector<std::string> paths;
		std::vector<std::pair<GameData::FormID, std::size_t>> grasses;
		for (const auto& [formID, grass] : a_snapshot.grassesByFormID) {
			auto path = GameData::NormalizeModelPath(grass.modelPath);
			if (path.empty()) {
				continue;
			}
			const auto [it, inserted] = modelIndexByPath.try_emplace(path, paths.size());
			if (inserted) {
				paths.push_back(std::move(path));
			}
			grasses.emplace_back(formID, it->second);
		}

		enum class Outcome : std::uint8_t
		{
			Measured,
			Missing,
			Unsupported
		};
		std::vector<std::uint32_t> blades(paths.size(), kMaxBladesPerBlock);
		std::vector<Outcome> outcomes(paths.size(), Outcome::Measured);
		oneapi::tbb::parallel_for(std::size_t{ 0 }, paths.size(), [&](std::size_t i) {
			const auto bytes = a_resolver.Read(paths[i]);
			if (!bytes) {
				outcomes[i] = Outcome::Missing;
				return;
			}
			const auto counts = Collision::ReadInstanceShapeCounts(*bytes);
			if (!counts) {
				outcomes[i] = Outcome::Unsupported;
				return;
			}
			blades[i] = BladesPerBlock(counts->triangles, counts->vertices);
		});

		GrassModelLayout layout;
		layout.models = paths.size();
		for (std::size_t i = 0; i < paths.size(); ++i) {
			if (outcomes[i] == Outcome::Missing) {
				layout.missing.push_back(paths[i]);
			} else if (outcomes[i] == Outcome::Unsupported) {
				layout.unsupported.push_back(paths[i]);
			}
		}
		std::ranges::sort(layout.missing);
		std::ranges::sort(layout.unsupported);
		for (const auto& [formID, model] : grasses) {
			layout.bladesPerBlock.emplace(formID, blades[model]);
			if (outcomes[model] == Outcome::Missing) {
				layout.missingGrass.insert(formID);
			}
		}
		return layout;
	}
}
