// Grass placement is a port of SARP's GrassCacheGenerator (vanilla paint mode), split so that
// in-object rejection can run between placement and serialization.

#include "Grass/Placement.h"

#include <oneapi/tbb/parallel_for.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <unordered_set>

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

		// The grass types the engine takes from a land texture, in GNAM order: only GNAMs that resolve
		// to a GRAS count, and the list ends once the count exceeds iMaxGrassTypesPerTexure (the
		// engine tests `count > max` before taking each one), so the default of 2 yields 3 types.
		template <class Visit>
		void ForEachTextureGrass(const GameData::StaticWorldSnapshot& a_snapshot, const GameData::LandTextureInfo& a_texture, std::uint32_t a_maxTypes,
			Visit&& a_visit)
		{
			std::uint32_t used = 0;
			for (const auto grassFormID : a_texture.grassFormIDs) {
				const auto grassIt = a_snapshot.grassesByFormID.find(grassFormID);
				if (grassIt == a_snapshot.grassesByFormID.end()) {
					continue;
				}
				if (used > a_maxTypes) {
					break;
				}
				++used;
				if (!grassIt->second.modelPath.empty()) {
					a_visit(grassIt->second);
				}
			}
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

				ForEachTextureGrass(a_snapshot, ltexIt->second, a_settings.maxGrassTypesPerTexture, [&](const GameData::GrassInfo& a_grass) {
					GrassParamBuild param;
					param.grass = std::addressof(a_grass);
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
				});
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
			float a_heightRandom)
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
			words[13] = FloatToHalfBits(a_heightRandom * a_grass.heightRange);
			words[14] = 0;
			words[15] = 0;

			a_blade.position[0] = a_x;
			a_blade.position[1] = a_y;
			a_blade.position[2] = a_sample.height;
		}
	}

	namespace
	{
		// ---- Smooth placement -------------------------------------------------------------------

		// Counter-based randomness: order-independent, so smooth placement needs no shared stream.
		[[nodiscard]] std::uint64_t Mix64(std::uint64_t a_value)
		{
			a_value += 0x9e3779b97f4a7c15ull;
			a_value = (a_value ^ (a_value >> 30)) * 0xbf58476d1ce4e5b9ull;
			a_value = (a_value ^ (a_value >> 27)) * 0x94d049bb133111ebull;
			return a_value ^ (a_value >> 31);
		}

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
			return Lerp(Lerp(LatticeValue(x0, y0, a_seed), LatticeValue(x0 + 1, y0, a_seed), fx),
				Lerp(LatticeValue(x0, y0 + 1, a_seed), LatticeValue(x0 + 1, y0 + 1, a_seed), fx), fy);
		}

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

		// Lattice points per cell side for a grass type: vanilla's pitch (iMinGrassSize, GRAS position
		// range), retiled so whole cells divide evenly and neighbouring cells' lattices meet without
		// gaps or overlap. Zero when the type places nothing.
		[[nodiscard]] std::uint32_t SmoothLatticeSide(const GameData::GrassInfo& a_grass, const PlacementSettings& a_settings)
		{
			const auto patchDiameter = static_cast<float>(a_settings.grassPatchSize * 2u);
			const auto countByMinGrass = a_settings.minGrassSize == 0 ? 0u : static_cast<std::uint32_t>(patchDiameter / static_cast<float>(a_settings.minGrassSize));
			const auto countByPositionRange = a_grass.positionRange <= 0.0f ? 0u : static_cast<std::uint32_t>(patchDiameter / a_grass.positionRange);
			const auto countSide = (std::min)(countByMinGrass, countByPositionRange);
			if (countSide == 0 || patchDiameter <= 0.0f) {
				return 0;
			}
			return (std::max)(1u, static_cast<std::uint32_t>(std::lround(GameData::kSkyrimTerrainCellSize * static_cast<float>(countSide) / patchDiameter)));
		}

		// One grass type's weights over the 3x3 block of cells around the cell being placed (97x97
		// vertices). A vertex shared by several LANDs holds their mean.
		constexpr std::size_t kBlockSide = 3 * 32 + 1;

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
					const auto origin = static_cast<std::size_t>(dy + 1) * 32 * kBlockSide + static_cast<std::size_t>(dx + 1) * 32;
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

		// a_localX/Y in vertices from the centre cell's origin; reads up to a cell into neighbours.
		[[nodiscard]] float SampleBlockGrid(const BlockWeightGrid& a_grid, float a_localX, float a_localY)
		{
			const auto lx = std::clamp(a_localX + 32.0f, 0.0f, 96.0f);
			const auto ly = std::clamp(a_localY + 32.0f, 0.0f, 96.0f);
			const auto x0 = (std::min)(static_cast<int>(lx), 95);
			const auto y0 = (std::min)(static_cast<int>(ly), 95);
			const auto fx = lx - static_cast<float>(x0);
			const auto fy = ly - static_cast<float>(y0);
			const auto at = [&](int x, int y) { return a_grid.weights[static_cast<std::size_t>(y) * kBlockSide + static_cast<std::size_t>(x)]; };
			return Lerp(Lerp(at(x0, y0), at(x0 + 1, y0), fx), Lerp(at(x0, y0 + 1), at(x0 + 1, y0 + 1), fx), fy);
		}

		CellCandidates GenerateSmoothCellCandidates(const GameData::StaticWorldSnapshot& a_snapshot, const LandInfo& a_land, const PlacementSettings& a_settings)
		{
			CellCandidates result;
			const auto cellX = *a_land.cellX;
			const auto cellY = *a_land.cellY;
			result.cellX = cellX;
			result.cellY = cellY;
			auto& counters = result.counters;
			const auto& smooth = a_settings.smooth;
			const CoverageRamp coverageOf(a_settings);

			std::optional<float> waterHeight = a_settings.waterHeight;
			if (const auto cellIt = a_snapshot.cellsByFormID.find(a_land.parentCell); cellIt != a_snapshot.cellsByFormID.end() && cellIt->second.waterHeight) {
				waterHeight = *cellIt->second.waterHeight;
			}
			const auto cellOriginX = static_cast<float>(cellX) * GameData::kSkyrimTerrainCellSize;
			const auto cellOriginY = static_cast<float>(cellY) * GameData::kSkyrimTerrainCellSize;
			const auto blockBaseX = static_cast<float>((cellX / 12) * 12) * GameData::kSkyrimTerrainCellSize;
			const auto blockBaseY = static_cast<float>((cellY / 12) * 12) * GameData::kSkyrimTerrainCellSize;
			// The warp field, sampled once per cell every 64 units and interpolated: shared by every grass
			// type, and far cheaper than evaluating the noise per lattice point.
			constexpr int kWarpSide = 65;
			constexpr float kWarpPitch = GameData::kSkyrimTerrainCellSize / static_cast<float>(kWarpSide - 1);
			const bool warp = smooth.warpAmplitude > 0.0f && smooth.warpWavelength > 0.0f;
			std::vector<float> warpX;
			std::vector<float> warpY;
			if (warp) {
				warpX.resize(kWarpSide * kWarpSide);
				warpY.resize(kWarpSide * kWarpSide);
				for (int j = 0; j < kWarpSide; ++j) {
					for (int i = 0; i < kWarpSide; ++i) {
						const auto wx = (cellOriginX + static_cast<float>(i) * kWarpPitch) / smooth.warpWavelength;
						const auto wy = (cellOriginY + static_cast<float>(j) * kWarpPitch) / smooth.warpWavelength;
						warpX[j * kWarpSide + i] = WarpNoise(wx, wy, 1u) * smooth.warpAmplitude;
						warpY[j * kWarpSide + i] = WarpNoise(wx, wy, 2u) * smooth.warpAmplitude;
					}
				}
			}
			const auto warpAt = [&](const std::vector<float>& a_field, float a_x, float a_y) {
				const auto gx = std::clamp((a_x - cellOriginX) / kWarpPitch, 0.0f, static_cast<float>(kWarpSide - 1));
				const auto gy = std::clamp((a_y - cellOriginY) / kWarpPitch, 0.0f, static_cast<float>(kWarpSide - 1));
				const auto x0 = (std::min)(static_cast<int>(gx), kWarpSide - 2);
				const auto y0 = (std::min)(static_cast<int>(gy), kWarpSide - 2);
				const auto fx = gx - static_cast<float>(x0);
				const auto fy = gy - static_cast<float>(y0);
				const auto at = [&](int x, int y) { return a_field[static_cast<std::size_t>(y * kWarpSide + x)]; };
				return Lerp(Lerp(at(x0, y0), at(x0 + 1, y0), fx), Lerp(at(x0, y0 + 1), at(x0 + 1, y0 + 1), fx), fy);
			};

			for (const auto& grid : BuildBlockWeightGrids(a_snapshot, a_land, a_settings)) {
				const auto& grass = *grid.grass;
				// Skip types with no coverage within a vertex of this cell.
				bool present = false;
				for (std::size_t y = 31; y <= 65 && !present; ++y) {
					for (std::size_t x = 31; x <= 65; ++x) {
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
				result.groups.push_back(CellGrassGroup{ .grass = std::addressof(grass), .modelPath = GameData::NormalizeModelPath(grass.modelPath) });
				CounterRng rng((static_cast<std::uint64_t>(static_cast<std::uint32_t>(cellX)) << 32) ^ static_cast<std::uint32_t>(cellY) ^
				               (static_cast<std::uint64_t>(grass.formID.value) * 0x9e3779b97f4a7c15ull));
				const auto density = static_cast<float>(grass.density) * 0.01f * (smooth.field ? smooth.field->DensityScale(grass.formID) : 1.0f);

				for (std::uint32_t latticeY = 0; latticeY < latticeSide; ++latticeY) {
					for (std::uint32_t latticeX = 0; latticeX < latticeSide; ++latticeX) {
						++counters.latticeCandidates;
						auto x = cellOriginX + (static_cast<float>(latticeX) + 0.5f + rng.Signed() * 0.5f) * step;
						auto y = cellOriginY + (static_cast<float>(latticeY) + 0.5f + rng.Signed() * 0.5f) * step;
						const auto accept = rng.Unit();
						// Coverage never exceeds 1, so most rejections need no weight lookup.
						if (accept >= density) {
							++counters.densityRejected;
							continue;
						}
						float lookupX = x;
						float lookupY = y;
						if (warp) {
							lookupX += warpAt(warpX, x, y);
							lookupY += warpAt(warpY, x, y);
						}
						const auto weight = SampleBlockGrid(grid, (lookupX - cellOriginX) / 128.0f, (lookupY - cellOriginY) / 128.0f);
						if (accept >= density * coverageOf(weight)) {
							++counters.densityRejected;
							continue;
						}
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
						const auto colorRandom = rng.Signed() * grass.colorRange;
						const auto brightness = Clamp01(
							(terrain.color[0] * 0.299f + terrain.color[1] * 0.587f + terrain.color[2] * 0.114f) * (1.0f - grass.colorRange + colorRandom));
						const auto orientation = rng.Signed();
						const auto heightRandom = rng.Signed();
						auto& blade = result.blades.emplace_back();
						blade.groupIndex = groupIndex;
						EncodeBlade(blade, cellX, cellY, x, y, terrain, grass, brightness, orientation, heightRandom);
						++counters.bladesPlaced;
					}
				}
			}
			return result;
		}
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
			if (const auto ltexIt = a_snapshot.landTexturesByFormID.find(a_texture); ltexIt != a_snapshot.landTexturesByFormID.end()) {
				ForEachTextureGrass(a_snapshot, ltexIt->second, a_settings.maxGrassTypesPerTexture, [&](const GameData::GrassInfo& a_grass) {
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
		for (std::uint8_t quadrant = 0; quadrant < LandInfo::QuadrantCount; ++quadrant) {
			const auto baseX = (quadrant & 1u) ? 16u : 0u;
			const auto baseY = (quadrant & 2u) ? 16u : 0u;
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

	namespace
	{
		// Expected blades of one LAND per grass type, before the water and slope filters (which both
		// modes apply alike): vanilla's exact lattice sum, and smooth placement's coverage integrated
		// over the cell (without the warp, which only moves weight around).
		[[nodiscard]] std::vector<std::pair<GameData::FormID, SmoothWeightField::ExpectedBlades>> ExpectedLandBlades(
			const GameData::StaticWorldSnapshot& a_snapshot,
			const LandInfo& a_land,
			std::span<const SmoothWeightField::Grid> a_grids,
			const PlacementSettings& a_settings)
		{
			std::vector<std::pair<GameData::FormID, SmoothWeightField::ExpectedBlades>> result;
			const auto entry = [&](GameData::FormID a_grass) -> SmoothWeightField::ExpectedBlades& {
				const auto it = std::ranges::find_if(result, [&](const auto& e) { return e.first == a_grass; });
				return it != result.end() ? it->second : result.emplace_back(a_grass, SmoothWeightField::ExpectedBlades{}).second;
			};

			// Vanilla: each lattice point accepts with the bilinear density of its patch's 3x3 grid. The
			// sum over a patch separates into per-axis sums of the three hat functions.
			const auto quadrantWeights = BuildVanillaQuadrantWeights(a_land);
			const auto evalStep = (std::max<std::uint32_t>)(1u, a_settings.grassEvalSize * 2u);
			const auto patchDiameter = static_cast<float>(a_settings.grassPatchSize * 2u);
			for (std::uint8_t quadrant = 0; quadrant < LandInfo::QuadrantCount; ++quadrant) {
				for (std::uint32_t vertexX = a_settings.grassEvalSize; vertexX < 16u; vertexX += evalStep) {
					for (std::uint32_t vertexY = a_settings.grassEvalSize; vertexY < 16u; vertexY += evalStep) {
						const auto sample = static_cast<std::size_t>(vertexY) * LandInfo::QuadrantVertexSide + vertexX;
						for (const auto& param : BuildGrassParamsForSample(a_snapshot, quadrantWeights, quadrant, sample, a_settings)) {
							const auto countByMinGrass = a_settings.minGrassSize == 0 ? 0u : static_cast<std::uint32_t>(patchDiameter / static_cast<float>(a_settings.minGrassSize));
							const auto countByPositionRange = param.grass->positionRange <= 0.0f ? 0u : static_cast<std::uint32_t>(patchDiameter / param.grass->positionRange);
							const auto countSide = (std::min)(countByMinGrass, countByPositionRange);
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
							entry(param.grass->formID).vanilla += blades;
						}
					}
				}
			}

			// Smooth: lattice points per cell times the cell's mean acceptance, from 64x64 samples.
			const CoverageRamp coverageOf(a_settings);
			for (const auto& grid : a_grids) {
				const auto latticeSide = SmoothLatticeSide(*grid.grass, a_settings);
				if (latticeSide == 0) {
					continue;
				}
				double sum = 0.0;
				for (int sy = 0; sy < 64; ++sy) {
					const auto ly = (static_cast<float>(sy) + 0.5f) * 0.5f;
					const auto y0 = (std::min)(static_cast<int>(ly), 31);
					const auto fy = ly - static_cast<float>(y0);
					for (int sx = 0; sx < 64; ++sx) {
						const auto lx = (static_cast<float>(sx) + 0.5f) * 0.5f;
						const auto x0 = (std::min)(static_cast<int>(lx), 31);
						const auto fx = lx - static_cast<float>(x0);
						const auto at = [&](int x, int y) { return static_cast<float>(grid.weights[static_cast<std::size_t>(y) * LandInfo::VertexSide + static_cast<std::size_t>(x)]) * (1.0f / 255.0f); };
						sum += coverageOf(Lerp(Lerp(at(x0, y0), at(x0 + 1, y0), fx), Lerp(at(x0, y0 + 1), at(x0 + 1, y0 + 1), fx), fy));
					}
				}
				const auto density = static_cast<double>(grid.grass->density) * 0.01;
				entry(grid.grass->formID).smooth += density * (sum / (64.0 * 64.0)) * static_cast<double>(latticeSide) * static_cast<double>(latticeSide);
			}
			return result;
		}
	}

	SmoothWeightField::SmoothWeightField(const GameData::StaticWorldSnapshot& a_snapshot, std::span<const LandInfo> a_worldLands,
		const PlacementSettings& a_settings)
	{
		// The first LAND of a cell wins, as everywhere else.
		std::vector<std::pair<std::uint64_t, const LandInfo*>> work;
		std::unordered_set<std::uint64_t> seen;
		for (const auto& land : a_worldLands) {
			if (land.cellX && land.cellY && land.hasHeights) {
				const auto cell = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(*land.cellX)) << 32) | static_cast<std::uint32_t>(*land.cellY);
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
		const auto it = _grids.find((static_cast<std::uint64_t>(static_cast<std::uint32_t>(a_cellX)) << 32) | static_cast<std::uint32_t>(a_cellY));
		return it != _grids.end() ? &it->second : nullptr;
	}

	CellCandidates GenerateCellCandidates(const GameData::StaticWorldSnapshot& a_snapshot, const LandInfo& a_land, const PlacementSettings& a_settings)
	{
		if (a_settings.mode == PlacementMode::Smooth && a_land.cellX && a_land.cellY && a_land.hasHeights) {
			return GenerateSmoothCellCandidates(a_snapshot, a_land, a_settings);
		}
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
								const auto heightRandom = RandomUnitSigned(rng.Next(0xffffffffu));
								auto& blade = result.blades.emplace_back();
								blade.groupIndex = groupIndex;
								EncodeBlade(blade, cellX, cellY, x, y, terrain, grass, brightness, orientation, heightRandom);
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
