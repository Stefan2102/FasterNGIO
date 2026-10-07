// Smooth placement: each blade's density comes from the texture weights at its own (optionally
// warped) position, so edges follow the painted terrain instead of vanilla's patch, quadrant and
// cell grids. Calibrated per grass type to vanilla's worldspace totals.

#include "Grass/SmoothPlacement.h"

#include "GameData/ModelPath.h"
#include "Grass/Internal/PlacementCommon.h"

#include <oneapi/tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace FasterNGIO::Grass
{
	namespace
	{
		using namespace Internal;

		// The golden-ratio increment of SplitMix64, also used to spread seeds.
		constexpr std::uint64_t kGoldenGamma = 0x9e3779b97f4a7c15ull;

		// SplitMix64's finalizer.
		[[nodiscard]] std::uint64_t Mix64(std::uint64_t a_value)
		{
			a_value += kGoldenGamma;
			a_value = (a_value ^ (a_value >> 30)) * 0xbf58476d1ce4e5b9ull;
			a_value = (a_value ^ (a_value >> 27)) * 0x94d049bb133111ebull;
			return a_value ^ (a_value >> 31);
		}

		// Counter-based randomness: order-independent, so smooth placement needs no shared stream.
		class CounterRng
		{
		public:
			explicit CounterRng(std::uint64_t a_seed) :
				_state(Mix64(a_seed)) {}

			// Uniform in [0, 1).
			[[nodiscard]] float Unit() { return static_cast<float>(Mix64(_state++) >> 40) * (1.0f / 16777216.0f); }
			// Uniform in [-1, 1).
			[[nodiscard]] float Signed() { return Unit() * 2.0f - 1.0f; }

		private:
			std::uint64_t _state;
		};

		[[nodiscard]] float LatticeValue(std::int32_t a_x, std::int32_t a_y, std::uint32_t a_seed)
		{
			const auto key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(a_x)) << 32) ^ static_cast<std::uint32_t>(a_y) ^
			                 (static_cast<std::uint64_t>(a_seed) * 0x632be59bd9b4e019ull);
			return static_cast<float>(Mix64(key) >> 40) * (2.0f / 16777216.0f) - 1.0f;
		}

		// Smoothly interpolated value noise in [-1, 1], in world coordinates so it is continuous
		// across cells.
		[[nodiscard]] float ValueNoise(float a_x, float a_y, std::uint32_t a_seed)
		{
			const auto x0 = static_cast<std::int32_t>(std::floor(a_x));
			const auto y0 = static_cast<std::int32_t>(std::floor(a_y));
			const auto smooth = [](float t) { return t * t * (3.0f - 2.0f * t); };
			const auto fx = smooth(a_x - static_cast<float>(x0));
			const auto fy = smooth(a_y - static_cast<float>(y0));
			return Bilerp(LatticeValue(x0, y0, a_seed), LatticeValue(x0 + 1, y0, a_seed), LatticeValue(x0, y0 + 1, a_seed), LatticeValue(x0 + 1, y0 + 1, a_seed), fx, fy);
		}

		// Two octaves of value noise; the second is offset so the octaves do not line up.
		[[nodiscard]] float WarpNoise(float a_x, float a_y, std::uint32_t a_seed)
		{
			return 0.65f * ValueNoise(a_x, a_y, a_seed) + 0.35f * ValueNoise(a_x * 2.0f + 17.3f, a_y * 2.0f - 5.1f, a_seed + 101u);
		}

		[[nodiscard]] float SmoothStep(float a_low, float a_high, float a_value)
		{
			if (a_high <= a_low) {
				return a_value > a_low ? 1.0f : 0.0f;
			}
			const auto t = Clamp01((a_value - a_low) / (a_high - a_low));
			return t * t * (3.0f - 2.0f * t);
		}

		// The share of a grass type's density placed at a texture weight.
		struct CoverageRamp
		{
			float low{ 0.0f };
			float high{ 0.0f };
			float bias{ 1.0f };

			// fTexturePctThreshold still applies: weights at or below it never grow grass.
			explicit CoverageRamp(const PlacementSettings& a_settings) :
				low((std::max)(a_settings.smooth.coverageLow, std::clamp(a_settings.alphaThreshold, 0.0f, 1.0f))),
				high((std::max)(a_settings.smooth.coverageHigh, low)),
				bias(a_settings.smooth.densityBias) {}

			[[nodiscard]] float operator()(float a_weight) const { return (std::min)(1.0f, bias * SmoothStep(low, high, a_weight)); }
		};

		// Lattice points per cell side for a grass type: vanilla's pitch, retiled so whole cells
		// divide evenly and neighbouring cells' lattices meet without gaps or overlap. Zero when the
		// type places nothing.
		[[nodiscard]] std::uint32_t SmoothLatticeSide(const GameData::GrassInfo& a_grass, const PlacementSettings& a_settings)
		{
			const auto patchDiameter = static_cast<float>(a_settings.grassPatchSize * 2u);
			const auto countSide = PatchLatticeSide(a_grass, a_settings);
			if (countSide == 0 || patchDiameter <= 0.0f) {
				return 0;
			}
			return (std::max)(1u, static_cast<std::uint32_t>(std::lround(GameData::kSkyrimTerrainCellSize * static_cast<float>(countSide) / patchDiameter)));
		}

		// One grass type's weights over the 3x3 block of cells around the cell being placed (97x97
		// vertices). A vertex shared by several LANDs holds their mean.
		constexpr std::size_t kCellQuads = LandInfo::VertexSide - 1;
		constexpr std::size_t kBlockSide = 3 * kCellQuads + 1;

		struct BlockWeightGrid
		{
			const GameData::GrassInfo* grass{ nullptr };
			std::vector<float> weights;
		};

		[[nodiscard]] std::vector<BlockWeightGrid> BuildBlockWeightGrids(const GameData::StaticWorldSnapshot& a_snapshot, const LandInfo& a_land,
			const PlacementSettings& a_settings)
		{
			const auto* field = a_settings.smooth.field;
			std::vector<SmoothWeightField::Grid> uncached;
			std::vector<BlockWeightGrid> result;
			std::vector<std::uint8_t> coverage(kBlockSide * kBlockSide, 0);
			std::unordered_map<GameData::FormID, std::size_t, GameData::FormIDHash> indexByGrass;
			for (int dy = -1; dy <= 1; ++dy) {
				for (int dx = -1; dx <= 1; ++dx) {
					const std::vector<SmoothWeightField::Grid>* grids = nullptr;
					if (field) {
						grids = field->Find(*a_land.cellX + dx, *a_land.cellY + dy);
					} else if (dx == 0 && dy == 0) {
						uncached = BuildSmoothWeightGrids(a_snapshot, a_land, a_settings);
						grids = &uncached;
					}
					if (!grids) {
						continue;
					}
					const auto origin = static_cast<std::size_t>(dy + 1) * kCellQuads * kBlockSide + static_cast<std::size_t>(dx + 1) * kCellQuads;
					for (std::size_t y = 0; y < LandInfo::VertexSide; ++y) {
						for (std::size_t x = 0; x < LandInfo::VertexSide; ++x) {
							++coverage[origin + y * kBlockSide + x];
						}
					}
					for (const auto& grid : *grids) {
						const auto [it, inserted] = indexByGrass.try_emplace(grid.grass->formID, result.size());
						if (inserted) {
							result.push_back(BlockWeightGrid{ .grass = grid.grass, .weights = std::vector<float>(kBlockSide * kBlockSide, 0.0f) });
						}
						auto& block = result[it->second].weights;
						for (std::size_t y = 0; y < LandInfo::VertexSide; ++y) {
							for (std::size_t x = 0; x < LandInfo::VertexSide; ++x) {
								block[origin + y * kBlockSide + x] += static_cast<float>(grid.weights[y * LandInfo::VertexSide + x]) * (1.0f / 255.0f);
							}
						}
					}
				}
			}
			for (auto& grid : result) {
				for (std::size_t i = 0; i < grid.weights.size(); ++i) {
					grid.weights[i] = coverage[i] ? grid.weights[i] / static_cast<float>(coverage[i]) : 0.0f;
				}
			}
			std::ranges::sort(result, [](const BlockWeightGrid& a, const BlockWeightGrid& b) { return a.grass->formID < b.grass->formID; });
			return result;
		}

		// Bilinear lookup in a square grid of a_side x a_side values, a_x and a_y in grid steps and
		// clamped to it.
		[[nodiscard]] float SampleGrid(const float* a_values, std::size_t a_side, float a_x, float a_y)
		{
			const auto last = static_cast<float>(a_side - 1);
			const auto gx = std::clamp(a_x, 0.0f, last);
			const auto gy = std::clamp(a_y, 0.0f, last);
			const auto x0 = (std::min)(static_cast<int>(gx), static_cast<int>(a_side) - 2);
			const auto y0 = (std::min)(static_cast<int>(gy), static_cast<int>(a_side) - 2);
			const auto at = [&](int x, int y) { return a_values[static_cast<std::size_t>(y) * a_side + static_cast<std::size_t>(x)]; };
			return Bilerp(at(x0, y0), at(x0 + 1, y0), at(x0, y0 + 1), at(x0 + 1, y0 + 1), gx - static_cast<float>(x0), gy - static_cast<float>(y0));
		}

		// The domain warp of one cell, sampled every 64 units and interpolated: shared by every grass
		// type, and far cheaper than evaluating the noise per lattice point.
		class CellWarp
		{
		public:
			CellWarp(float a_cellOriginX, float a_cellOriginY, const SmoothPlacementSettings& a_smooth) :
				_originX(a_cellOriginX), _originY(a_cellOriginY), _enabled(a_smooth.warpAmplitude > 0.0f && a_smooth.warpWavelength > 0.0f)
			{
				if (!_enabled) {
					return;
				}
				_x.resize(kSide * kSide);
				_y.resize(kSide * kSide);
				for (std::size_t j = 0; j < kSide; ++j) {
					for (std::size_t i = 0; i < kSide; ++i) {
						const auto wx = (_originX + static_cast<float>(i) * kPitch) / a_smooth.warpWavelength;
						const auto wy = (_originY + static_cast<float>(j) * kPitch) / a_smooth.warpWavelength;
						_x[j * kSide + i] = WarpNoise(wx, wy, 1u) * a_smooth.warpAmplitude;
						_y[j * kSide + i] = WarpNoise(wx, wy, 2u) * a_smooth.warpAmplitude;
					}
				}
			}

			// Moves a world position by the warp (unchanged when the warp is off).
			void Apply(float& a_x, float& a_y) const
			{
				if (!_enabled) {
					return;
				}
				const auto gx = (a_x - _originX) / kPitch;
				const auto gy = (a_y - _originY) / kPitch;
				a_x += SampleGrid(_x.data(), kSide, gx, gy);
				a_y += SampleGrid(_y.data(), kSide, gx, gy);
			}

		private:
			static constexpr std::size_t kSide = 65;
			static constexpr float kPitch = GameData::kSkyrimTerrainCellSize / static_cast<float>(kSide - 1);

			float _originX;
			float _originY;
			bool _enabled;
			std::vector<float> _x;
			std::vector<float> _y;
		};

		// Expected blades of one LAND per grass type: vanilla's exact lattice sum, and smooth
		// placement's coverage integrated over the cell (without the warp, which only moves weight
		// around).
		[[nodiscard]] std::vector<std::pair<GameData::FormID, SmoothWeightField::ExpectedBlades>> ExpectedLandBlades(const GameData::StaticWorldSnapshot& a_snapshot,
			const LandInfo& a_land, std::span<const SmoothWeightField::Grid> a_grids, const PlacementSettings& a_settings)
		{
			std::vector<std::pair<GameData::FormID, SmoothWeightField::ExpectedBlades>> result;
			const auto entry = [&](GameData::FormID a_grass) -> SmoothWeightField::ExpectedBlades& {
				const auto it = std::ranges::find_if(result, [&](const auto& e) { return e.first == a_grass; });
				return it != result.end() ? it->second : result.emplace_back(a_grass, SmoothWeightField::ExpectedBlades{}).second;
			};
			AddExpectedVanillaBlades(a_snapshot, a_land, a_settings, [&](GameData::FormID a_grass, double a_blades) { entry(a_grass).vanilla += a_blades; });

			// Lattice points per cell times the cell's mean acceptance, from 64x64 samples.
			constexpr int kSamples = 64;
			constexpr float kSampleToVertex = static_cast<float>(kCellQuads) / static_cast<float>(kSamples);
			const CoverageRamp coverageOf(a_settings);
			for (const auto& grid : a_grids) {
				const auto latticeSide = SmoothLatticeSide(*grid.grass, a_settings);
				if (latticeSide == 0) {
					continue;
				}
				double sum = 0.0;
				for (int sy = 0; sy < kSamples; ++sy) {
					const auto ly = (static_cast<float>(sy) + 0.5f) * kSampleToVertex;
					const auto y0 = (std::min)(static_cast<int>(ly), static_cast<int>(kCellQuads) - 1);
					const auto fy = ly - static_cast<float>(y0);
					for (int sx = 0; sx < kSamples; ++sx) {
						const auto lx = (static_cast<float>(sx) + 0.5f) * kSampleToVertex;
						const auto x0 = (std::min)(static_cast<int>(lx), static_cast<int>(kCellQuads) - 1);
						const auto fx = lx - static_cast<float>(x0);
						const auto at = [&](int x, int y) { return static_cast<float>(grid.weights[static_cast<std::size_t>(y) * LandInfo::VertexSide + static_cast<std::size_t>(x)]) * (1.0f / 255.0f); };
						sum += coverageOf(Bilerp(at(x0, y0), at(x0 + 1, y0), at(x0, y0 + 1), at(x0 + 1, y0 + 1), fx, fy));
					}
				}
				// The percentage in double precision (not kDensityPercent, a float).
				const auto density = static_cast<double>(grid.grass->density) * 0.01;
				entry(grid.grass->formID).smooth += density * (sum / (static_cast<double>(kSamples) * kSamples)) * static_cast<double>(latticeSide) * static_cast<double>(latticeSide);
			}
			return result;
		}
	}

	CellCandidates Internal::GenerateSmoothCellCandidates(const GameData::StaticWorldSnapshot& a_snapshot, const LandInfo& a_land, const PlacementSettings& a_settings)
	{
		CellCandidates result;
		const auto cellX = *a_land.cellX;
		const auto cellY = *a_land.cellY;
		result.cellX = cellX;
		result.cellY = cellY;
		const auto& smooth = a_settings.smooth;
		const CoverageRamp coverageOf(a_settings);
		const auto waterHeight = CellWaterHeight(a_snapshot, a_land, a_settings);
		const auto cellOriginX = static_cast<float>(cellX) * GameData::kSkyrimTerrainCellSize;
		const auto cellOriginY = static_cast<float>(cellY) * GameData::kSkyrimTerrainCellSize;
		const CellWarp warp(cellOriginX, cellOriginY, smooth);

		for (const auto& grid : BuildBlockWeightGrids(a_snapshot, a_land, a_settings)) {
			const auto& grass = *grid.grass;
			// Skip types with no coverage within a vertex of this cell.
			bool present = false;
			for (std::size_t y = kCellQuads - 1; y <= 2 * kCellQuads + 1 && !present; ++y) {
				for (std::size_t x = kCellQuads - 1; x <= 2 * kCellQuads + 1; ++x) {
					if (grid.weights[y * kBlockSide + x] > coverageOf.low) {
						present = true;
						break;
					}
				}
			}
			if (!present) {
				continue;
			}
			const auto latticeSide = SmoothLatticeSide(grass, a_settings);
			if (latticeSide == 0) {
				continue;
			}
			const auto step = GameData::kSkyrimTerrainCellSize / static_cast<float>(latticeSide);
			const auto groupIndex = static_cast<std::uint32_t>(result.groups.size());
			result.groups.push_back(CellGrassGroup{ .grass = std::addressof(grass), .modelPath = GameData::GrassCacheModelPath(grass.modelPath) });
			CounterRng rng(GameData::PackCellCoords(cellX, cellY) ^ (static_cast<std::uint64_t>(grass.formID.value) * kGoldenGamma));
			const auto density = static_cast<float>(grass.density) * kDensityPercent * (smooth.field ? smooth.field->DensityScale(grass.formID) : 1.0f);

			for (std::uint32_t latticeY = 0; latticeY < latticeSide; ++latticeY) {
				for (std::uint32_t latticeX = 0; latticeX < latticeSide; ++latticeX) {
					const auto x = cellOriginX + (static_cast<float>(latticeX) + 0.5f + rng.Signed() * 0.5f) * step;
					const auto y = cellOriginY + (static_cast<float>(latticeY) + 0.5f + rng.Signed() * 0.5f) * step;
					const auto accept = rng.Unit();
					// Coverage never exceeds 1, so most rejections need no weight lookup.
					if (accept >= density) {
						continue;
					}
					float lookupX = x;
					float lookupY = y;
					warp.Apply(lookupX, lookupY);
					const auto weight = SampleGrid(grid.weights.data(), kBlockSide, (lookupX - cellOriginX) / kVertexSpacing + static_cast<float>(kCellQuads),
						(lookupY - cellOriginY) / kVertexSpacing + static_cast<float>(kCellQuads));
					if (accept >= density * coverageOf(weight)) {
						continue;
					}
					constexpr float kHalfCell = GameData::kSkyrimTerrainCellSize * 0.5f;
					const auto quadrant = static_cast<std::uint8_t>((x - cellOriginX >= kHalfCell ? 1u : 0u) | (y - cellOriginY >= kHalfCell ? 2u : 0u));
					EmitBlade(result, groupIndex, quadrant, a_land, grass, x, y, waterHeight, [&] { return rng.Signed(); });
				}
			}
		}
		return result;
	}

	std::vector<SmoothWeightField::Grid> BuildSmoothWeightGrids(const GameData::StaticWorldSnapshot& a_snapshot, const LandInfo& a_land, const PlacementSettings& a_settings)
	{
		const auto quadrantWeights = BuildVanillaQuadrantWeights(a_land);
		std::array<std::uint8_t, LandInfo::VertexCount> coverage{};
		std::vector<std::array<float, LandInfo::VertexCount>> sums;
		std::vector<SmoothWeightField::Grid> grids;
		std::unordered_map<GameData::FormID, std::size_t, GameData::FormIDHash> gridByGrass;
		std::unordered_map<GameData::FormID, std::vector<std::size_t>, GameData::FormIDHash> gridsByTexture;
		const auto gridsFor = [&](GameData::FormID a_texture) -> const std::vector<std::size_t>& {
			if (const auto it = gridsByTexture.find(a_texture); it != gridsByTexture.end()) {
				return it->second;
			}
			std::vector<std::size_t> indices;
			if (const auto* texture = GrassListTexture(a_snapshot, a_settings, a_texture)) {
				ForEachTextureGrass(a_snapshot, *texture, a_settings, [&](const GameData::GrassInfo& a_grass) {
					const auto [gridIt, inserted] = gridByGrass.try_emplace(a_grass.formID, grids.size());
					if (inserted) {
						grids.push_back(SmoothWeightField::Grid{ .grass = std::addressof(a_grass) });
						sums.emplace_back().fill(0.0f);
					}
					if (std::ranges::find(indices, gridIt->second) == indices.end()) {
						indices.push_back(gridIt->second);
					}
				});
			}
			return gridsByTexture.emplace(a_texture, std::move(indices)).first->second;
		};

		// Shared quadrant-edge vertices average the quadrants' weights, so the four quadrants'
		// independent layer sets do not produce seams.
		constexpr auto kQuadrantQuads = LandInfo::QuadrantVertexSide - 1;
		for (std::uint8_t quadrant = 0; quadrant < LandInfo::QuadrantCount; ++quadrant) {
			const auto baseX = (quadrant & 1u) ? kQuadrantQuads : 0u;
			const auto baseY = (quadrant & 2u) ? kQuadrantQuads : 0u;
			for (std::size_t y = 0; y < LandInfo::QuadrantVertexSide; ++y) {
				for (std::size_t x = 0; x < LandInfo::QuadrantVertexSide; ++x) {
					const auto vertex = (baseY + y) * LandInfo::VertexSide + baseX + x;
					++coverage[vertex];
					for (const auto& sample : quadrantWeights[quadrant][y * LandInfo::QuadrantVertexSide + x]) {
						for (const auto grid : gridsFor(sample.formID)) {
							sums[grid][vertex] += sample.weight;
						}
					}
				}
			}
		}
		for (std::size_t g = 0; g < grids.size(); ++g) {
			for (std::size_t vertex = 0; vertex < LandInfo::VertexCount; ++vertex) {
				const auto weight = coverage[vertex] ? Clamp01(sums[g][vertex] / static_cast<float>(coverage[vertex])) : 0.0f;
				grids[g].weights[vertex] = static_cast<std::uint8_t>(std::lround(weight * 255.0f));
			}
		}
		std::ranges::sort(grids, [](const SmoothWeightField::Grid& a, const SmoothWeightField::Grid& b) { return a.grass->formID < b.grass->formID; });
		return grids;
	}

	SmoothWeightField::SmoothWeightField(const GameData::StaticWorldSnapshot& a_snapshot, std::span<const LandInfo> a_worldLands, const PlacementSettings& a_settings)
	{
		// The first LAND of a cell wins, as everywhere else.
		std::vector<std::pair<std::uint64_t, const LandInfo*>> work;
		std::unordered_set<std::uint64_t> seen;
		for (const auto& land : a_worldLands) {
			if (land.cellX && land.cellY && land.hasHeights) {
				const auto cell = GameData::PackCellCoords(*land.cellX, *land.cellY);
				if (seen.insert(cell).second) {
					work.emplace_back(cell, std::addressof(land));
				}
			}
		}
		std::vector<std::vector<Grid>> built(work.size());
		// Expected blades per land and grass type, reduced after the parallel pass (no shared state).
		using Expectations = std::vector<std::pair<GameData::FormID, ExpectedBlades>>;
		std::vector<Expectations> expected(a_settings.smooth.matchVanillaDensity ? work.size() : 0);
		oneapi::tbb::parallel_for(std::size_t{ 0 }, work.size(), [&](std::size_t i) {
			built[i] = BuildSmoothWeightGrids(a_snapshot, *work[i].second, a_settings);
			if (a_settings.smooth.matchVanillaDensity) {
				expected[i] = ExpectedLandBlades(a_snapshot, *work[i].second, built[i], a_settings);
			}
		});
		_grids.reserve(work.size());
		for (std::size_t i = 0; i < work.size(); ++i) {
			_gridCount += built[i].size();
			_grids.emplace(work[i].first, std::move(built[i]));
		}
		for (const auto& land : expected) {
			for (const auto& [grass, blades] : land) {
				auto& total = _expected[grass];
				total.vanilla += blades.vanilla;
				total.smooth += blades.smooth;
			}
		}
		for (const auto& [grass, total] : _expected) {
			if (total.smooth > 0.0 && total.vanilla > 0.0) {
				// Bounded so a type that barely appears in one mode cannot run away.
				_densityScale.emplace(grass, static_cast<float>(std::clamp(total.vanilla / total.smooth, 0.25, 4.0)));
			}
		}
	}

	float SmoothWeightField::DensityScale(GameData::FormID a_grass) const
	{
		const auto it = _densityScale.find(a_grass);
		return it != _densityScale.end() ? it->second : 1.0f;
	}

	const std::vector<SmoothWeightField::Grid>* SmoothWeightField::Find(std::int32_t a_cellX, std::int32_t a_cellY) const
	{
		const auto it = _grids.find(GameData::PackCellCoords(a_cellX, a_cellY));
		return it != _grids.end() ? &it->second : nullptr;
	}
}
