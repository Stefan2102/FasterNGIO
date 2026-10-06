// The engine's grass placement, reproduced bit for bit (RNG draw order included): a 3x3 density
// grid around every few quadrant vertices, and a jittered lattice of candidates per patch.

#include "GameData/ModelPath.h"
#include "Grass/Internal/PlacementCommon.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace FasterNGIO::Grass::Internal
{
	namespace
	{
		// The engine's Mersenne-Twister RNG (MT19937 constants), including its modulo/mask quirk.
		class SkyrimRng
		{
		public:
			explicit SkyrimRng(std::uint32_t a_seed)
			{
				_state[0] = a_seed;
				for (std::uint32_t i = 1; i < _state.size(); ++i) {
					const auto prev = _state[i - 1];
					_state[i] = (((prev >> 30) + i) ^ prev) * 0x6c078965u;
				}
				_index = kStateSize - 1;
			}

			// A value below a_max, or masked with it when a_max + 1 is a power of two (as the engine does).
			[[nodiscard]] std::uint32_t Next(std::uint32_t a_max)
			{
				if (a_max == 0) {
					return 0;
				}
				if (_index == 0) {
					Twist();
				}
				auto value = _state[_index];
				_index = (_index + 1) % kStateSize;
				value ^= value >> 11;
				value ^= (value & 0xff3a58adu) << 7;
				value ^= (value & 0xffffdf8cu) << 15;
				value ^= value >> 18;
				if (((a_max + 1u) & 0xffff7fffu) != 0) {
					return value % a_max;
				}
				return value & a_max;
			}

		private:
			static constexpr std::uint32_t kStateSize = 624;
			static constexpr std::uint32_t kShift = 397;

			void Twist()
			{
				for (std::uint32_t i = 0; i < kStateSize; ++i) {
					const auto next = _state[(i + 1u) % kStateSize];
					const auto mix = next % kStateSize + (_state[i] & 1u);
					auto value = _state[(i + kShift) % kStateSize] ^ (mix * 2u);
					if ((mix & 1u) != 0) {
						value ^= 0x9908b0dfu;
					}
					_state[i] = value;
				}
				_index = 0;
			}

			std::array<std::uint32_t, kStateSize> _state{};
			std::uint32_t _index{ 0 };
		};

		// A full 32-bit draw as a value in [-1, 1).
		[[nodiscard]] float RandomUnitSigned(std::uint32_t a_value)
		{
			return static_cast<float>(a_value) * 4.6566128730773926e-10f - 1.0f;
		}

		// The quadrant vertices the engine evaluates grass at: every 2 * grassEvalSize vertices,
		// starting at grassEvalSize.
		template <class Visit>
		void ForEachEvaluatedVertex(const PlacementSettings& a_settings, Visit&& a_visit)
		{
			const auto evalStep = (std::max<std::uint32_t>)(1u, a_settings.grassEvalSize * 2u);
			for (std::uint8_t quadrant = 0; quadrant < LandInfo::QuadrantCount; ++quadrant) {
				for (std::uint32_t vertexX = a_settings.grassEvalSize; vertexX < 16u; vertexX += evalStep) {
					for (std::uint32_t vertexY = a_settings.grassEvalSize; vertexY < 16u; vertexY += evalStep) {
						a_visit(quadrant, vertexX, vertexY, static_cast<std::size_t>(vertexY) * LandInfo::QuadrantVertexSide + vertexX);
					}
				}
			}
		}
	}

	CellCandidates GenerateVanillaCellCandidates(const GameData::StaticWorldSnapshot& a_snapshot, const LandInfo& a_land, const PlacementSettings& a_settings)
	{
		CellCandidates result;
		const auto cellX = *a_land.cellX;
		const auto cellY = *a_land.cellY;
		result.cellX = cellX;
		result.cellY = cellY;

		const auto quadrantWeights = BuildVanillaQuadrantWeights(a_land);
		std::unordered_map<GameData::FormID, std::uint32_t, GameData::FormIDHash> groupIndexByGrass;
		SkyrimRng rng(static_cast<std::uint32_t>(cellX * cellY));
		const auto waterHeight = CellWaterHeight(a_snapshot, a_land, a_settings);
		const auto patchDiameter = static_cast<float>(a_settings.grassPatchSize * 2u);
		const auto signedRandom = [&] { return RandomUnitSigned(rng.Next(0xffffffffu)); };

		ForEachEvaluatedVertex(a_settings, [&](std::uint8_t a_quadrant, std::uint32_t a_vertexX, std::uint32_t a_vertexY, std::size_t a_sample) {
			const auto params = BuildGrassParamsForSample(a_snapshot, quadrantWeights, a_quadrant, a_sample, a_settings);
			if (params.empty()) {
				return;
			}
			const auto quadBaseX = ((a_quadrant & 1u) ? 16 : 0);
			const auto quadBaseY = ((a_quadrant & 2u) ? 16 : 0);
			// In whole game units, as the engine computes it.
			constexpr int kCellUnits = static_cast<int>(GameData::kSkyrimTerrainCellSize);
			constexpr int kVertexUnits = static_cast<int>(kVertexSpacing);
			const auto vertexWorldX = static_cast<float>(cellX * kCellUnits + (quadBaseX + static_cast<int>(a_vertexX)) * kVertexUnits);
			const auto vertexWorldY = static_cast<float>(cellY * kCellUnits + (quadBaseY + static_cast<int>(a_vertexY)) * kVertexUnits);
			const auto patchStartX = vertexWorldX - static_cast<float>(a_settings.grassPatchSize);
			const auto patchStartY = vertexWorldY - static_cast<float>(a_settings.grassPatchSize);

			for (const auto& param : params) {
				const auto& grass = *param.grass;
				const auto countSide = PatchLatticeSide(grass, a_settings);
				if (countSide == 0) {
					continue;
				}
				const auto cellStep = patchDiameter / static_cast<float>(countSide);
				const auto halfStep = cellStep * 0.5f;

				const auto [groupIt, inserted] = groupIndexByGrass.try_emplace(grass.formID, static_cast<std::uint32_t>(result.groups.size()));
				if (inserted) {
					result.groups.push_back(CellGrassGroup{ .grass = std::addressof(grass), .modelPath = GameData::NormalizeModelPath(grass.modelPath) });
				}
				const auto groupIndex = groupIt->second;

				for (std::uint32_t latticeY = 0; latticeY < countSide; ++latticeY) {
					const auto v = (static_cast<float>(latticeY) + 0.5f) * (2.0f / static_cast<float>(countSide));
					const auto iy = std::clamp(static_cast<int>(std::floor(v)), 0, 1);
					const auto fy = v - static_cast<float>(iy);
					for (std::uint32_t latticeX = 0; latticeX < countSide; ++latticeX) {
						const auto u = (static_cast<float>(latticeX) + 0.5f) * (2.0f / static_cast<float>(countSide));
						const auto ix = std::clamp(static_cast<int>(std::floor(u)), 0, 1);
						const auto fx = u - static_cast<float>(ix);
						const auto d00 = param.density[static_cast<std::size_t>(iy) * 3 + static_cast<std::size_t>(ix)];
						const auto d10 = param.density[static_cast<std::size_t>(iy) * 3 + static_cast<std::size_t>(ix + 1)];
						const auto d01 = param.density[static_cast<std::size_t>(iy + 1) * 3 + static_cast<std::size_t>(ix)];
						const auto d11 = param.density[static_cast<std::size_t>(iy + 1) * 3 + static_cast<std::size_t>(ix + 1)];
						// Acceptance is a 15-bit draw against the density in 1/32768ths.
						const auto threshold = static_cast<std::uint32_t>(Bilerp(d00, d10, d01, d11, fx, fy) * 32768.0f);
						if (threshold == 0 || rng.Next(0x7fffu) >= threshold) {
							continue;
						}

						const auto jitterX = RandomUnitSigned(rng.Next(0xffffffffu)) * halfStep;
						const auto jitterY = RandomUnitSigned(rng.Next(0xffffffffu)) * halfStep;
						const auto x = patchStartX + (static_cast<float>(latticeX) + 0.5f) * cellStep + jitterX;
						const auto y = patchStartY + (static_cast<float>(latticeY) + 0.5f) * cellStep + jitterY;
						// NGIO's in-object test runs inside this in the engine. It is applied as a
						// post-filter instead, so the colour/orientation/height draws are always consumed.
						EmitBlade(result, groupIndex, a_quadrant, a_land, grass, x, y, waterHeight, signedRandom);
					}
				}
			}
		});
		return result;
	}

	void AddExpectedVanillaBlades(const GameData::StaticWorldSnapshot& a_snapshot, const LandInfo& a_land, const PlacementSettings& a_settings,
		const std::function<void(GameData::FormID, double)>& a_add)
	{
		// Each lattice point accepts with the bilinear density of its patch's 3x3 grid. The sum over a
		// patch separates into per-axis sums of the three hat functions.
		const auto quadrantWeights = BuildVanillaQuadrantWeights(a_land);
		ForEachEvaluatedVertex(a_settings, [&](std::uint8_t a_quadrant, std::uint32_t, std::uint32_t, std::size_t a_sample) {
			for (const auto& param : BuildGrassParamsForSample(a_snapshot, quadrantWeights, a_quadrant, a_sample, a_settings)) {
				const auto countSide = PatchLatticeSide(*param.grass, a_settings);
				if (countSide == 0) {
					continue;
				}
				std::array<double, 3> hat{};
				for (std::uint32_t i = 0; i < countSide; ++i) {
					const auto u = (static_cast<double>(i) + 0.5) * (2.0 / static_cast<double>(countSide));
					hat[0] += (std::max)(0.0, 1.0 - u);
					hat[1] += 1.0 - std::abs(u - 1.0);
					hat[2] += (std::max)(0.0, u - 1.0);
				}
				double blades = 0.0;
				for (std::size_t b = 0; b < 3; ++b) {
					for (std::size_t a = 0; a < 3; ++a) {
						blades += static_cast<double>(param.density[b * 3 + a]) * hat[a] * hat[b];
					}
				}
				a_add(param.grass->formID, blades);
			}
		});
	}
}
