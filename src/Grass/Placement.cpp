// Grass placement is a port of SARP's GrassCacheGenerator (vanilla paint mode), split so that
// in-object rejection can run between placement and serialization.

#include "Grass/Placement.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <unordered_map>

namespace FasterNGIO::Grass
{
	namespace
	{
		using GameData::LandInfo;

		struct TerrainSample
		{
			float height{ 0.0f };
			float normal[3]{ 0.0f, 0.0f, 1.0f };
			float color[3]{ 1.0f, 1.0f, 1.0f };
		};

		struct GrassParamBuild
		{
			const GameData::GrassInfo* grass{ nullptr };
			std::array<float, 9> density{};
		};

		struct TextureSampleWeight
		{
			GameData::FormID formID{};
			float weight{ 0.0f };
			bool isBaseTexture{ false };
			std::uint16_t layerIndex{ 0 };
		};

		using QuadrantSampleWeights = std::array<std::array<std::vector<TextureSampleWeight>, LandInfo::QuadrantVertexCount>, LandInfo::QuadrantCount>;

		// Emulation of the engine's Mersenne-Twister RNG, including its modulo/mask quirk.
		class SkyrimRng
		{
		public:
			explicit SkyrimRng(std::uint32_t a_seed)
			{
				_state[0] = a_seed;
				for (std::uint32_t i = 1; i < _state.size(); ++i) {
					const auto prev = _state[i - 1];
					_state[i] = ((prev >> 30) + i ^ prev) * 0x6c078965u;
				}
				_index = 0x26f;
			}

			[[nodiscard]] std::uint32_t Next(std::uint32_t a_max)
			{
				if (a_max == 0) {
					return 0;
				}
				if (_index == 0) {
					Twist();
				}
				auto value = _state[_index];
				_index = (_index + 1) % 0x270u;
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
			void Twist()
			{
				for (std::uint32_t i = 0; i < 0x270u; ++i) {
					const auto next = _state[(i + 1u) % 0x270u];
					const auto mix = next % 0x270u + (_state[i] & 1u);
					auto value = _state[(i + 0x18du) % 0x270u] ^ (mix * 2u);
					if ((mix & 1u) != 0) {
						value ^= 0x9908b0dfu;
					}
					_state[i] = value;
				}
				_index = 0;
			}

			std::array<std::uint32_t, 0x270> _state{};
			std::uint32_t _index{ 0 };
		};

		[[nodiscard]] std::uint16_t FloatToHalfBits(float a_value)
		{
			const auto bits = std::bit_cast<std::uint32_t>(a_value);
			const auto sign = static_cast<std::uint16_t>((bits >> 16) & 0x8000u);
			auto exponent = static_cast<int>((bits >> 23) & 0xFFu) - 127 + 15;
			auto mantissa = bits & 0x7FFFFFu;
			if (exponent <= 0) {
				if (exponent < -10) {
					return sign;
				}
				mantissa |= 0x800000u;
				const auto shift = static_cast<std::uint32_t>(14 - exponent);
				auto halfMantissa = static_cast<std::uint16_t>(mantissa >> shift);
				if ((mantissa >> (shift - 1)) & 1u) {
					++halfMantissa;
				}
				return sign | halfMantissa;
			}
			if (exponent >= 31) {
				return sign | 0x7C00u;
			}
			auto half = static_cast<std::uint16_t>(sign | (static_cast<std::uint16_t>(exponent) << 10) | static_cast<std::uint16_t>(mantissa >> 13));
			if (mantissa & 0x1000u) {
				++half;
			}
			return half;
		}

		[[nodiscard]] float HalfBitsToFloat(std::uint16_t a_value)
		{
			const auto sign = static_cast<std::uint32_t>(a_value & 0x8000u) << 16;
			auto exponent = static_cast<std::uint32_t>((a_value >> 10) & 0x1Fu);
			auto mantissa = static_cast<std::uint32_t>(a_value & 0x03FFu);
			if (exponent == 0) {
				if (mantissa == 0) {
					return std::bit_cast<float>(sign);
				}
				while ((mantissa & 0x0400u) == 0) {
					mantissa <<= 1;
					--exponent;
				}
				++exponent;
				mantissa &= 0x03FFu;
			} else if (exponent == 31) {
				return std::bit_cast<float>(sign | 0x7F800000u | (mantissa << 13));
			}
			exponent = exponent + (127 - 15);
			return std::bit_cast<float>(sign | (exponent << 23) | (mantissa << 13));
		}

		[[nodiscard]] float Clamp01(float a_value)
		{
			return (std::min)((std::max)(a_value, 0.0f), 1.0f);
		}

		[[nodiscard]] float NormalizeOpacity(float a_opacity)
		{
			return Clamp01(a_opacity > 1.0f ? a_opacity * 0.01f : a_opacity);
		}

		[[nodiscard]] float RandomUnitSigned(std::uint32_t a_value)
		{
			return static_cast<float>(a_value) * 4.6566128730773926e-10f - 1.0f;
		}

		[[nodiscard]] float Lerp(float a_lhs, float a_rhs, float a_t)
		{
			return a_lhs + (a_rhs - a_lhs) * a_t;
		}

		[[nodiscard]] const std::vector<TextureSampleWeight>& SampleWeightsAt(const QuadrantSampleWeights& a_weights, std::uint8_t a_quadrant, int a_x, int a_y)
		{
			const auto clampedX = std::clamp(a_x, 0, static_cast<int>(LandInfo::QuadrantVertexSide - 1));
			const auto clampedY = std::clamp(a_y, 0, static_cast<int>(LandInfo::QuadrantVertexSide - 1));
			return a_weights[a_quadrant][static_cast<std::size_t>(clampedY) * LandInfo::QuadrantVertexSide + static_cast<std::size_t>(clampedX)];
		}

		[[nodiscard]] float WeightForTexture(const QuadrantSampleWeights& a_weights, std::uint8_t a_quadrant, int a_x, int a_y, GameData::FormID a_landTextureFormID)
		{
			float weight = 0.0f;
			for (const auto& sampleWeight : SampleWeightsAt(a_weights, a_quadrant, a_x, a_y)) {
				if (sampleWeight.formID == a_landTextureFormID) {
					weight += sampleWeight.weight;
				}
			}
			return Clamp01(weight);
		}

		[[nodiscard]] QuadrantSampleWeights BuildVanillaQuadrantWeights(const LandInfo& a_land)
		{
			QuadrantSampleWeights result;
			for (std::uint8_t quadrant = 0; quadrant < LandInfo::QuadrantCount; ++quadrant) {
				const auto baseFormID = a_land.baseTextures[quadrant] ? a_land.baseTextures[quadrant]->landTextureFormID : GameData::kSkyrimDefaultLandTextureFormID;

				std::array<GameData::FormID, 6> alphaFormIDs{};
				std::array<std::array<float, LandInfo::QuadrantVertexCount>, 6> alphaWeights{};
				for (auto& weights : alphaWeights) {
					weights.fill(0.0f);
				}
				for (const auto& alpha : a_land.alphaTextures) {
					if (alpha.quadrant == quadrant && alpha.layerIndex < alphaFormIDs.size()) {
						alphaFormIDs[alpha.layerIndex] = alpha.landTextureFormID;
					}
				}
				for (const auto& alpha : a_land.vertexAlphas) {
					if (alpha.quadrant == quadrant && alpha.layerIndex < alphaWeights.size() && alpha.position < LandInfo::QuadrantVertexCount) {
						alphaWeights[alpha.layerIndex][alpha.position] = NormalizeOpacity(alpha.opacity);
					}
				}

				for (std::size_t sample = 0; sample < LandInfo::QuadrantVertexCount; ++sample) {
					float alphaCoverage = 0.0f;
					for (std::size_t slot = 0; slot < alphaWeights.size(); ++slot) {
						const auto weight = alphaWeights[slot][sample];
						if (!alphaFormIDs[slot].IsNull() && alphaFormIDs[slot] != baseFormID && weight > 0.0f) {
							result[quadrant][sample].push_back(TextureSampleWeight{
								.formID = alphaFormIDs[slot],
								.weight = weight,
								.isBaseTexture = false,
								.layerIndex = static_cast<std::uint16_t>(slot),
							});
							alphaCoverage += weight;
						}
					}
					const auto baseWeight = Clamp01(1.0f - alphaCoverage);
					if (baseWeight > 0.0f) {
						result[quadrant][sample].push_back(TextureSampleWeight{
							.formID = baseFormID,
							.weight = baseWeight,
							.isBaseTexture = true,
							.layerIndex = 0xffffu,
						});
					}
				}
			}
			return result;
		}

		[[nodiscard]] std::array<float, 3> TerrainFaceNormal(int a_x0, int a_y0, float a_h0, int a_x1, int a_y1, float a_h1, int a_x2, int a_y2, float a_h2)
		{
			const float p10[3]{ static_cast<float>(a_x1 - a_x0) * 128.0f, static_cast<float>(a_y1 - a_y0) * 128.0f, a_h1 - a_h0 };
			const float p20[3]{ static_cast<float>(a_x2 - a_x0) * 128.0f, static_cast<float>(a_y2 - a_y0) * 128.0f, a_h2 - a_h0 };
			std::array<float, 3> normal{
				p10[1] * p20[2] - p10[2] * p20[1],
				p10[2] * p20[0] - p10[0] * p20[2],
				p10[0] * p20[1] - p10[1] * p20[0]
			};
			auto length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
			if (length > 1.0e-6f) {
				if (normal[2] < 0.0f) {
					length = -length;
				}
				normal[0] /= length;
				normal[1] /= length;
				normal[2] /= length;
			} else {
				normal = { 0.0f, 0.0f, 1.0f };
			}
			return normal;
		}

		// Uses the engine's alternating triangle split of each 128-unit LAND quad.
		[[nodiscard]] TerrainSample SampleTerrain(const LandInfo& a_land, float a_x, float a_y)
		{
			TerrainSample result;
			if (!a_land.cellX || !a_land.cellY || !a_land.hasHeights) {
				return result;
			}

			const auto localX = std::clamp((a_x - static_cast<float>(*a_land.cellX * 4096)) / 128.0f, 0.0f, 32.0f);
			const auto localY = std::clamp((a_y - static_cast<float>(*a_land.cellY * 4096)) / 128.0f, 0.0f, 32.0f);
			const auto x0 = std::clamp(static_cast<int>(std::floor(localX)), 0, 31);
			const auto y0 = std::clamp(static_cast<int>(std::floor(localY)), 0, 31);
			const auto x1 = x0 + 1;
			const auto y1 = y0 + 1;
			const auto fx = localX - static_cast<float>(x0);
			const auto fy = localY - static_cast<float>(y0);
			const auto sampleHeight = [&](int x, int y) {
				return a_land.heights[static_cast<std::size_t>(y) * LandInfo::VertexSide + static_cast<std::size_t>(x)];
			};
			const auto h00 = sampleHeight(x0, y0);
			const auto h10 = sampleHeight(x1, y0);
			const auto h01 = sampleHeight(x0, y1);
			const auto h11 = sampleHeight(x1, y1);

			struct Vertex
			{
				int x{ 0 };
				int y{ 0 };
				float h{ 0.0f };
			};
			Vertex v0;
			Vertex v1;
			Vertex v2;
			float w0 = 0.0f;
			float w1 = 0.0f;
			float w2 = 0.0f;
			if (((x0 + y0) & 1) == 0) {
				if (fx > fy) {
					v0 = { x0, y0, h00 };
					v1 = { x1, y0, h10 };
					v2 = { x1, y1, h11 };
					w0 = 1.0f - fx;
					w1 = fx - fy;
					w2 = fy;
				} else {
					v0 = { x0, y0, h00 };
					v1 = { x1, y1, h11 };
					v2 = { x0, y1, h01 };
					w0 = 1.0f - fy;
					w1 = fx;
					w2 = fy - fx;
				}
			} else {
				if (fx + fy <= 1.0f) {
					v0 = { x0, y0, h00 };
					v1 = { x1, y0, h10 };
					v2 = { x0, y1, h01 };
					w0 = 1.0f - fx - fy;
					w1 = fx;
					w2 = fy;
				} else {
					v0 = { x1, y1, h11 };
					v1 = { x0, y1, h01 };
					v2 = { x1, y0, h10 };
					w0 = fx + fy - 1.0f;
					w1 = 1.0f - fx;
					w2 = 1.0f - fy;
				}
			}
			result.height = v0.h * w0 + v1.h * w1 + v2.h * w2;
			const auto faceNormal = TerrainFaceNormal(v0.x, v0.y, v0.h, v1.x, v1.y, v1.h, v2.x, v2.y, v2.h);
			result.normal[0] = faceNormal[0];
			result.normal[1] = faceNormal[1];
			result.normal[2] = faceNormal[2];

			if (a_land.hasVertexColors) {
				const auto sampleColor = [&](const Vertex& v, std::size_t c) {
					return static_cast<float>(a_land.vertexColors[static_cast<std::size_t>(v.y) * LandInfo::VertexSide + static_cast<std::size_t>(v.x)][c]) / 255.0f;
				};
				for (std::size_t c = 0; c < 3; ++c) {
					result.color[c] = sampleColor(v0, c) * w0 + sampleColor(v1, c) * w1 + sampleColor(v2, c) * w2;
				}
			}
			return result;
		}

		[[nodiscard]] bool PassesWaterFilter(const GameData::GrassInfo& a_grass, float a_height, float a_waterHeight)
		{
			using GameData::GrassWaterState;
			const auto range = static_cast<float>(a_grass.distanceFromWaterLevel);
			switch (a_grass.underwaterState) {
			case GrassWaterState::AboveOnlyAtLeast:
				return a_height >= a_waterHeight - range;
			case GrassWaterState::AboveOnlyAtMost:
				return a_height >= a_waterHeight && a_height <= a_waterHeight + range;
			case GrassWaterState::BelowOnlyAtLeast:
				return a_height <= a_waterHeight - range;
			case GrassWaterState::BelowOnlyAtMost:
				return a_height >= a_waterHeight - range && a_height <= a_waterHeight;
			case GrassWaterState::BothAtLeast:
				return a_height >= a_waterHeight + range || a_height <= a_waterHeight - range;
			case GrassWaterState::BothAtMost:
				return a_height >= a_waterHeight - range && a_height <= a_waterHeight + range;
			case GrassWaterState::BothAtMostAbove:
				return a_height <= a_waterHeight + range;
			case GrassWaterState::BothAtMostBelow:
				return a_height >= a_waterHeight - range;
			default:
				return true;
			}
		}

		[[nodiscard]] bool PassesSlopeFilter(const GameData::GrassInfo& a_grass, const TerrainSample& a_sample)
		{
			constexpr float pi = 3.14159265358979323846f;
			const auto minCos = std::cos(static_cast<float>(a_grass.minSlopeDegrees) * pi / 180.0f);
			const auto maxCos = std::cos(static_cast<float>(a_grass.maxSlopeDegrees) * pi / 180.0f);
			return maxCos <= a_sample.normal[2] && a_sample.normal[2] <= minCos;
		}

		[[nodiscard]] std::vector<GrassParamBuild> BuildGrassParamsForSample(
			const GameData::StaticWorldSnapshot& a_snapshot,
			const QuadrantSampleWeights& a_weights,
			std::uint8_t a_quadrant,
			std::size_t a_sample,
			const PlacementSettings& a_settings)
		{
			struct CandidateTexture
			{
				GameData::FormID formID{};
				bool isBaseTexture{ false };
				std::uint16_t layerIndex{ 0 };
				std::size_t firstSeen{ 0 };
			};

			std::vector<GrassParamBuild> params;
			const auto centerX = static_cast<int>(a_sample % LandInfo::QuadrantVertexSide);
			const auto centerY = static_cast<int>(a_sample / LandInfo::QuadrantVertexSide);
			std::vector<CandidateTexture> candidates;
			std::size_t firstSeen = 0;
			for (int dy = -1; dy <= 1; ++dy) {
				for (int dx = -1; dx <= 1; ++dx) {
					for (const auto& sampleWeight : SampleWeightsAt(a_weights, a_quadrant, centerX + dx, centerY + dy)) {
						const auto existingIt = std::ranges::find_if(candidates, [&](const CandidateTexture& candidate) {
							return candidate.formID == sampleWeight.formID && candidate.isBaseTexture == sampleWeight.isBaseTexture &&
							       candidate.layerIndex == sampleWeight.layerIndex;
						});
						if (existingIt == candidates.end()) {
							candidates.push_back(CandidateTexture{
								.formID = sampleWeight.formID,
								.isBaseTexture = sampleWeight.isBaseTexture,
								.layerIndex = sampleWeight.layerIndex,
								.firstSeen = firstSeen++,
							});
						}
					}
				}
			}
			std::ranges::stable_sort(candidates, [](const CandidateTexture& lhs, const CandidateTexture& rhs) {
				if (lhs.isBaseTexture != rhs.isBaseTexture) {
					return lhs.isBaseTexture;
				}
				if (lhs.layerIndex != rhs.layerIndex) {
					return lhs.layerIndex < rhs.layerIndex;
				}
				return lhs.firstSeen < rhs.firstSeen;
			});

			const auto thresholdByte = static_cast<std::uint32_t>(std::clamp(a_settings.alphaThreshold, 0.0f, 1.0f) * 255.0f);
			const auto thresholdWeight = static_cast<float>(thresholdByte) / 255.0f;
			constexpr float alphaLayerNeighborGate = 25.0f / 255.0f;
			for (const auto& candidate : candidates) {
				if (!candidate.isBaseTexture) {
					bool hasSignificantNeighbor = false;
					for (int dy = -1; dy <= 1 && !hasSignificantNeighbor; ++dy) {
						for (int dx = -1; dx <= 1; ++dx) {
							if (WeightForTexture(a_weights, a_quadrant, centerX + dx, centerY + dy, candidate.formID) > alphaLayerNeighborGate) {
								hasSignificantNeighbor = true;
								break;
							}
						}
					}
					if (!hasSignificantNeighbor) {
						continue;
					}
				}
				const auto ltexIt = a_snapshot.landTexturesByFormID.find(candidate.formID);
				if (ltexIt == a_snapshot.landTexturesByFormID.end()) {
					continue;
				}

				std::uint32_t grassTypesUsed = 0;
				for (const auto grassFormID : ltexIt->second.grassFormIDs) {
					if (grassTypesUsed++ >= a_settings.maxGrassTypesPerTexture) {
						break;
					}
					const auto grassIt = a_snapshot.grassesByFormID.find(grassFormID);
					if (grassIt == a_snapshot.grassesByFormID.end() || grassIt->second.modelPath.empty()) {
						continue;
					}
					GrassParamBuild param;
					param.grass = std::addressof(grassIt->second);
					float sum = 0.0f;
					for (int dy = -1; dy <= 1; ++dy) {
						for (int dx = -1; dx <= 1; ++dx) {
							const auto index = static_cast<std::size_t>((dy + 1) * 3 + (dx + 1));
							const auto weight = WeightForTexture(a_weights, a_quadrant, centerX + dx, centerY + dy, candidate.formID);
							const bool enabled = candidate.isBaseTexture ? weight > a_settings.alphaThreshold : weight > thresholdWeight;
							param.density[index] = enabled ? static_cast<float>(param.grass->density) * 0.01f : 0.0f;
							sum += param.density[index];
						}
					}
					if (sum / 9.0f >= a_settings.alphaThreshold) {
						params.push_back(param);
					}
				}
			}
			return params;
		}

		void EncodeBlade(
			BladeCandidate& a_blade,
			std::int32_t a_cellX,
			std::int32_t a_cellY,
			float a_x,
			float a_y,
			const TerrainSample& a_sample,
			const GameData::GrassInfo& a_grass,
			float a_brightness,
			float a_orientation,
			SkyrimRng& a_rng)
		{
			auto& words = a_blade.words;
			const auto blockBaseX = static_cast<float>((a_cellX / 12) * 12) * GameData::kSkyrimTerrainCellSize;
			const auto blockBaseY = static_cast<float>((a_cellY / 12) * 12) * GameData::kSkyrimTerrainCellSize;
			words[0] = FloatToHalfBits(a_x - blockBaseX);
			words[1] = FloatToHalfBits(a_y - blockBaseY);
			words[2] = FloatToHalfBits(a_sample.height);
			words[3] = FloatToHalfBits(Clamp01(a_brightness));

			const auto sinTheta = std::clamp(a_orientation, -1.0f, 1.0f);
			const auto cosTheta = std::sqrt((std::max)(0.0f, 1.0f - sinTheta * sinTheta));
			float basis0[3]{ sinTheta, -cosTheta, 0.0f };
			float basis1[3]{ cosTheta, sinTheta, 0.0f };
			float basis2[3]{ 0.0f, 0.0f, 1.0f };
			if (a_grass.FitsToSlope()) {
				basis2[0] = a_sample.normal[0];
				basis2[1] = a_sample.normal[1];
				basis2[2] = a_sample.normal[2];
				const auto dot = basis0[0] * basis2[0] + basis0[1] * basis2[1] + basis0[2] * basis2[2];
				basis0[0] -= dot * basis2[0];
				basis0[1] -= dot * basis2[1];
				basis0[2] -= dot * basis2[2];
				const auto len = std::sqrt(basis0[0] * basis0[0] + basis0[1] * basis0[1] + basis0[2] * basis0[2]);
				if (len > 1.0e-6f) {
					basis0[0] /= len;
					basis0[1] /= len;
					basis0[2] /= len;
				}
				// Keep the same handedness as the non-slope basis: basis1 = basis2 x basis0.
				basis1[0] = basis2[1] * basis0[2] - basis2[2] * basis0[1];
				basis1[1] = basis2[2] * basis0[0] - basis2[0] * basis0[2];
				basis1[2] = basis2[0] * basis0[1] - basis2[1] * basis0[0];
			}

			words[4] = FloatToHalfBits(basis0[0]);
			words[5] = FloatToHalfBits(basis0[1]);
			words[6] = FloatToHalfBits(basis0[2]);
			words[7] = FloatToHalfBits(basis2[0]);
			words[8] = FloatToHalfBits(basis1[0]);
			words[9] = FloatToHalfBits(basis1[1]);
			words[10] = FloatToHalfBits(basis1[2]);
			words[11] = FloatToHalfBits(basis2[1]);
			words[12] = FloatToHalfBits(basis2[2]);
			words[13] = FloatToHalfBits(RandomUnitSigned(a_rng.Next(0xffffffffu)) * a_grass.heightRange);
			words[14] = 0;
			words[15] = 0;

			a_blade.position[0] = a_x;
			a_blade.position[1] = a_y;
			a_blade.position[2] = a_sample.height;
		}
	}

	CellCandidates GenerateCellCandidates(const GameData::StaticWorldSnapshot& a_snapshot, const LandInfo& a_land, const PlacementSettings& a_settings)
	{
		CellCandidates result;
		if (!a_land.cellX || !a_land.cellY || !a_land.hasHeights) {
			return result;
		}
		const auto cellX = *a_land.cellX;
		const auto cellY = *a_land.cellY;
		result.cellX = cellX;
		result.cellY = cellY;
		auto& counters = result.counters;

		const auto quadrantWeights = BuildVanillaQuadrantWeights(a_land);
		std::unordered_map<GameData::FormID, std::uint32_t, GameData::FormIDHash> groupIndexByGrass;
		SkyrimRng rng(static_cast<std::uint32_t>(cellX * cellY));
		std::optional<float> waterHeight = a_settings.waterHeight;
		if (const auto cellIt = a_snapshot.cellsByFormID.find(a_land.parentCell); cellIt != a_snapshot.cellsByFormID.end() && cellIt->second.waterHeight) {
			waterHeight = *cellIt->second.waterHeight;
		}
		const auto evalStep = (std::max<std::uint32_t>)(1u, a_settings.grassEvalSize * 2u);
		const auto patchDiameter = static_cast<float>(a_settings.grassPatchSize * 2u);
		const auto blockBaseX = static_cast<float>((cellX / 12) * 12) * GameData::kSkyrimTerrainCellSize;
		const auto blockBaseY = static_cast<float>((cellY / 12) * 12) * GameData::kSkyrimTerrainCellSize;

		for (std::uint8_t quadrant = 0; quadrant < LandInfo::QuadrantCount; ++quadrant) {
			const auto quadBaseX = ((quadrant & 1u) ? 16 : 0);
			const auto quadBaseY = ((quadrant & 2u) ? 16 : 0);
			for (std::uint32_t vertexX = a_settings.grassEvalSize; vertexX < 16u; vertexX += evalStep) {
				for (std::uint32_t vertexY = a_settings.grassEvalSize; vertexY < 16u; vertexY += evalStep) {
					const auto sample = static_cast<std::size_t>(vertexY) * LandInfo::QuadrantVertexSide + vertexX;
					++counters.samplesVisited;
					const auto params = BuildGrassParamsForSample(a_snapshot, quadrantWeights, quadrant, sample, a_settings);
					if (params.empty()) {
						continue;
					}
					++counters.samplesWithGrassParams;
					counters.grassParamsBuilt += params.size();

					const auto vertexWorldX = static_cast<float>(cellX * 4096 + (quadBaseX + static_cast<int>(vertexX)) * 128);
					const auto vertexWorldY = static_cast<float>(cellY * 4096 + (quadBaseY + static_cast<int>(vertexY)) * 128);
					const auto patchStartX = vertexWorldX - static_cast<float>(a_settings.grassPatchSize);
					const auto patchStartY = vertexWorldY - static_cast<float>(a_settings.grassPatchSize);

					for (const auto& param : params) {
						const auto& grass = *param.grass;
						const auto countByMinGrass = a_settings.minGrassSize == 0 ? 0u : static_cast<std::uint32_t>(patchDiameter / static_cast<float>(a_settings.minGrassSize));
						const auto countByPositionRange = grass.positionRange <= 0.0f ? 0u : static_cast<std::uint32_t>(patchDiameter / grass.positionRange);
						const auto countSide = (std::min)(countByMinGrass, countByPositionRange);
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
								const auto density = Lerp(Lerp(d00, d10, fx), Lerp(d01, d11, fx), fy);
								const auto threshold = static_cast<std::uint32_t>(density * 32768.0f);
								++counters.latticeCandidates;
								if (threshold == 0 || rng.Next(0x7fffu) >= threshold) {
									++counters.densityRejected;
									continue;
								}

								const auto jitterX = RandomUnitSigned(rng.Next(0xffffffffu)) * halfStep;
								const auto jitterY = RandomUnitSigned(rng.Next(0xffffffffu)) * halfStep;
								auto x = patchStartX + (static_cast<float>(latticeX) + 0.5f) * cellStep + jitterX;
								auto y = patchStartY + (static_cast<float>(latticeY) + 0.5f) * cellStep + jitterY;
								x = blockBaseX + HalfBitsToFloat(FloatToHalfBits(x - blockBaseX));
								y = blockBaseY + HalfBitsToFloat(FloatToHalfBits(y - blockBaseY));

								const auto terrain = SampleTerrain(a_land, x, y);
								if (waterHeight && !PassesWaterFilter(grass, terrain.height, *waterHeight)) {
									++counters.waterRejected;
									continue;
								}
								if (!PassesSlopeFilter(grass, terrain)) {
									++counters.slopeRejected;
									continue;
								}

								// NGIO's in-object test runs here in the engine. It is applied as a post-filter
								// instead, so the colour/orientation/height draws are always consumed.
								const auto colorRandom = RandomUnitSigned(rng.Next(0xffffffffu)) * grass.colorRange;
								const auto brightness = Clamp01(
									(terrain.color[0] * 0.299f + terrain.color[1] * 0.587f + terrain.color[2] * 0.114f) * (1.0f - grass.colorRange + colorRandom));
								const auto orientation = RandomUnitSigned(rng.Next(0xffffffffu));
								auto& blade = result.blades.emplace_back();
								blade.groupIndex = groupIndex;
								EncodeBlade(blade, cellX, cellY, x, y, terrain, grass, brightness, orientation, rng);
								++counters.bladesPlaced;
							}
						}
					}
				}
			}
		}
		return result;
	}

	NgioCellCache FinalizeCell(const CellCandidates& a_candidates, std::span<const std::uint32_t> a_rejected, std::uint32_t a_strideWords)
	{
		struct GroupBuild
		{
			std::vector<std::uint16_t> bladeWords;
			float min[3]{ (std::numeric_limits<float>::max)(), (std::numeric_limits<float>::max)(), (std::numeric_limits<float>::max)() };
			float max[3]{ -(std::numeric_limits<float>::max)(), -(std::numeric_limits<float>::max)(), -(std::numeric_limits<float>::max)() };
		};

		const auto strideWords = (std::min<std::uint32_t>)(a_strideWords, 16u);
		std::vector<GroupBuild> builds(a_candidates.groups.size());
		for (std::size_t i = 0; i < a_candidates.blades.size(); ++i) {
			if (!a_rejected.empty() && (a_rejected[i / 32] & (1u << (i % 32))) != 0) {
				continue;
			}
			const auto& blade = a_candidates.blades[i];
			auto& build = builds[blade.groupIndex];
			const auto& grass = *a_candidates.groups[blade.groupIndex].grass;
			build.bladeWords.insert(build.bladeWords.end(), blade.words.begin(), blade.words.begin() + strideWords);
			for (int axis = 0; axis < 2; ++axis) {
				build.min[axis] = (std::min)(build.min[axis], blade.position[axis]);
				build.max[axis] = (std::max)(build.max[axis], blade.position[axis]);
			}
			build.min[2] = (std::min)(build.min[2], blade.position[2]);
			build.max[2] = (std::max)(build.max[2], blade.position[2] + (std::max)(grass.heightRange, 1.0f));
		}

		std::vector<std::uint32_t> order;
		for (std::uint32_t i = 0; i < builds.size(); ++i) {
			if (!builds[i].bladeWords.empty()) {
				order.push_back(i);
			}
		}
		std::ranges::sort(order, [&](std::uint32_t lhs, std::uint32_t rhs) {
			return a_candidates.groups[lhs].grass->formID < a_candidates.groups[rhs].grass->formID;
		});

		NgioCellCache cache;
		for (const auto index : order) {
			const auto& source = a_candidates.groups[index];
			auto& build = builds[index];
			NgioGrassGroup group;
			group.modelPath = source.modelPath;
			group.grassFormID = source.grass->formID.value;
			group.vertexLighting = source.grass->HasVertexLighting();
			group.uniformScaling = source.grass->HasUniformScaling();
			group.fitToSlope = source.grass->FitsToSlope();
			NgioGrassGeometryBlock block;
			const auto bladeCount = static_cast<std::uint32_t>(build.bladeWords.size() / strideWords);
			block.descriptorWords = {
				std::bit_cast<std::uint32_t>(build.min[0]),
				std::bit_cast<std::uint32_t>(build.min[1]),
				std::bit_cast<std::uint32_t>(build.min[2]),
				std::bit_cast<std::uint32_t>(build.max[0]),
				std::bit_cast<std::uint32_t>(build.max[1]),
				std::bit_cast<std::uint32_t>(build.max[2]),
				0u,
				bladeCount,
				strideWords,
			};
			block.payloadWords = std::move(build.bladeWords);
			group.blocks.push_back(std::move(block));
			cache.groups.push_back(std::move(group));
		}
		return cache;
	}

	std::string MakeNgioCacheFileName(std::string_view a_worldEditorID, std::int32_t a_cellX, std::int32_t a_cellY)
	{
		return std::format("{}x{:04}y{:04}.cgid", a_worldEditorID, a_cellX, a_cellY);
	}

	std::string ResolveWorldEditorID(const GameData::StaticWorldSnapshot& a_snapshot, GameData::FormID a_worldFormID)
	{
		const auto worldIt = a_snapshot.worldsByFormID.find(a_worldFormID);
		if (worldIt != a_snapshot.worldsByFormID.end() && !worldIt->second.editorID.empty()) {
			return worldIt->second.editorID;
		}
		if (a_worldFormID.value == 0x0000003Cu) {
			return "Tamriel";
		}
		return std::format("{:08X}", a_worldFormID.value);
	}

	bool ExistingNgioCacheLooksValid(const std::filesystem::path& a_path)
	{
		std::error_code ec;
		const auto size = std::filesystem::file_size(a_path, ec);
		if (ec || size < sizeof(std::uint32_t)) {
			return false;
		}
		std::ifstream input(a_path, std::ios::binary);
		std::array<unsigned char, 4> bytes{};
		input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		if (!input) {
			return false;
		}
		const auto groupCount = static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8u) |
		                        (static_cast<std::uint32_t>(bytes[2]) << 16u) | (static_cast<std::uint32_t>(bytes[3]) << 24u);
		return groupCount <= 4096u;
	}
}
