#include "Grass/Placement.h"

#include "Grass/Internal/PlacementCommon.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace FasterNGIO::Grass
{
	namespace Internal
	{
		namespace
		{
			// LAND alpha opacities are stored either as 0-1 or as percentages.
			[[nodiscard]] float NormalizeOpacity(float a_opacity)
			{
				return Clamp01(a_opacity > 1.0f ? a_opacity * 0.01f : a_opacity);
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

			[[nodiscard]] std::array<float, 3> TerrainFaceNormal(int a_x0, int a_y0, float a_h0, int a_x1, int a_y1, float a_h1, int a_x2, int a_y2, float a_h2)
			{
				const float p10[3]{ static_cast<float>(a_x1 - a_x0) * kVertexSpacing, static_cast<float>(a_y1 - a_y0) * kVertexSpacing, a_h1 - a_h0 };
				const float p20[3]{ static_cast<float>(a_x2 - a_x0) * kVertexSpacing, static_cast<float>(a_y2 - a_y0) * kVertexSpacing, a_h2 - a_h0 };
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
		}

		std::uint16_t FloatToHalfBits(float a_value)
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

		float HalfBitsToFloat(std::uint16_t a_value)
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

		std::optional<float> CellWaterHeight(const GameData::StaticWorldSnapshot& a_snapshot, const LandInfo& a_land, const PlacementSettings& a_settings)
		{
			if (const auto cellIt = a_snapshot.cellsByFormID.find(a_land.parentCell); cellIt != a_snapshot.cellsByFormID.end() && cellIt->second.waterHeight) {
				return *cellIt->second.waterHeight;
			}
			return a_settings.waterHeight;
		}

		std::uint32_t PatchLatticeSide(const GameData::GrassInfo& a_grass, const PlacementSettings& a_settings)
		{
			const auto patchDiameter = static_cast<float>(a_settings.grassPatchSize * 2u);
			const auto countByMinGrass = a_settings.minGrassSize == 0 ? 0u : static_cast<std::uint32_t>(patchDiameter / static_cast<float>(a_settings.minGrassSize));
			const auto countByPositionRange = a_grass.positionRange <= 0.0f ? 0u : static_cast<std::uint32_t>(patchDiameter / a_grass.positionRange);
			return (std::min)(countByMinGrass, countByPositionRange);
		}

		QuadrantSampleWeights BuildVanillaQuadrantWeights(const LandInfo& a_land)
		{
			// LAND has a base texture and up to six alpha layers per quadrant.
			constexpr std::size_t kAlphaLayers = 6;
			// Marks the base texture in TextureSampleWeight::layerIndex.
			constexpr std::uint16_t kBaseLayer = 0xffffu;

			QuadrantSampleWeights result;
			for (std::uint8_t quadrant = 0; quadrant < LandInfo::QuadrantCount; ++quadrant) {
				const auto baseFormID = a_land.baseTextures[quadrant] ? a_land.baseTextures[quadrant]->landTextureFormID : GameData::kSkyrimDefaultLandTextureFormID;

				std::array<GameData::FormID, kAlphaLayers> alphaFormIDs{};
				std::array<std::array<float, LandInfo::QuadrantVertexCount>, kAlphaLayers> alphaWeights{};
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
							.layerIndex = kBaseLayer,
						});
					}
				}
			}
			return result;
		}

		TerrainSample SampleTerrain(const LandInfo& a_land, float a_x, float a_y)
		{
			constexpr auto kQuads = static_cast<int>(LandInfo::VertexSide - 1);
			TerrainSample result;
			if (!a_land.cellX || !a_land.cellY || !a_land.hasHeights) {
				return result;
			}

			const auto cellSize = static_cast<std::int32_t>(GameData::kSkyrimTerrainCellSize);
			const auto localX = std::clamp((a_x - static_cast<float>(*a_land.cellX * cellSize)) / kVertexSpacing, 0.0f, static_cast<float>(kQuads));
			const auto localY = std::clamp((a_y - static_cast<float>(*a_land.cellY * cellSize)) / kVertexSpacing, 0.0f, static_cast<float>(kQuads));
			const auto x0 = std::clamp(static_cast<int>(std::floor(localX)), 0, kQuads - 1);
			const auto y0 = std::clamp(static_cast<int>(std::floor(localY)), 0, kQuads - 1);
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
			// Quads alternate their diagonal like a checkerboard.
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

		bool PassesWaterFilter(const GameData::GrassInfo& a_grass, float a_height, float a_waterHeight)
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

		bool PassesSlopeFilter(const GameData::GrassInfo& a_grass, const TerrainSample& a_sample)
		{
			constexpr float pi = 3.14159265358979323846f;
			const auto minCos = std::cos(static_cast<float>(a_grass.minSlopeDegrees) * pi / 180.0f);
			const auto maxCos = std::cos(static_cast<float>(a_grass.maxSlopeDegrees) * pi / 180.0f);
			return maxCos <= a_sample.normal[2] && a_sample.normal[2] <= minCos;
		}

		std::vector<GrassParamBuild> BuildGrassParamsForSample(const GameData::StaticWorldSnapshot& a_snapshot, const QuadrantSampleWeights& a_weights,
			std::uint8_t a_quadrant, std::size_t a_sample, const PlacementSettings& a_settings)
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
			// An alpha layer only counts where some neighbouring vertex carries more than this weight.
			constexpr float kAlphaLayerNeighborGate = 25.0f / 255.0f;
			for (const auto& candidate : candidates) {
				if (!candidate.isBaseTexture) {
					bool hasSignificantNeighbor = false;
					for (int dy = -1; dy <= 1 && !hasSignificantNeighbor; ++dy) {
						for (int dx = -1; dx <= 1; ++dx) {
							if (WeightForTexture(a_weights, a_quadrant, centerX + dx, centerY + dy, candidate.formID) > kAlphaLayerNeighborGate) {
								hasSignificantNeighbor = true;
								break;
							}
						}
					}
					if (!hasSignificantNeighbor) {
						continue;
					}
				}
				const auto* texture = GrassListTexture(a_snapshot, a_settings, candidate.formID);
				if (!texture) {
					continue;
				}

				ForEachTextureGrass(a_snapshot, *texture, a_settings, [&](const GameData::GrassInfo& a_grass) {
					GrassParamBuild param;
					param.grass = std::addressof(a_grass);
					float sum = 0.0f;
					for (int dy = -1; dy <= 1; ++dy) {
						for (int dx = -1; dx <= 1; ++dx) {
							const auto index = static_cast<std::size_t>((dy + 1) * 3 + (dx + 1));
							const auto weight = WeightForTexture(a_weights, a_quadrant, centerX + dx, centerY + dy, candidate.formID);
							const bool enabled = candidate.isBaseTexture ? weight > a_settings.alphaThreshold : weight > thresholdWeight;
							param.density[index] = enabled ? static_cast<float>(param.grass->density) * kDensityPercent : 0.0f;
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

		void EncodeBlade(BladeCandidate& a_blade, std::int32_t a_cellX, std::int32_t a_cellY, float a_x, float a_y, const TerrainSample& a_sample,
			const GameData::GrassInfo& a_grass, float a_brightness, float a_orientation, float a_heightRandom, bool a_fitToSlope)
		{
			auto& words = a_blade.words;
			words[0] = FloatToHalfBits(a_x - BlockBase(a_cellX));
			words[1] = FloatToHalfBits(a_y - BlockBase(a_cellY));
			words[2] = FloatToHalfBits(a_sample.height);
			words[3] = FloatToHalfBits(Clamp01(a_brightness));

			const auto sinTheta = std::clamp(a_orientation, -1.0f, 1.0f);
			const auto cosTheta = std::sqrt((std::max)(0.0f, 1.0f - sinTheta * sinTheta));
			float basis0[3]{ sinTheta, -cosTheta, 0.0f };
			float basis1[3]{ cosTheta, sinTheta, 0.0f };
			float basis2[3]{ 0.0f, 0.0f, 1.0f };
			if (a_fitToSlope) {
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

			// The grass vertex shader reads words 4-15 as three float4s and rotates the model by
			// float3x3(words 4-6, words 8-10, (word 12, word 7, word 11)): those are the matrix rows,
			// so the basis vectors (where the model's x, y and up axes go) are its columns. The
			// shader also takes (word 6, word 10, word 11) as the blade's up vector for lighting.
			words[4] = FloatToHalfBits(basis0[0]);
			words[5] = FloatToHalfBits(basis1[0]);
			words[6] = FloatToHalfBits(basis2[0]);
			words[7] = FloatToHalfBits(basis1[2]);
			words[8] = FloatToHalfBits(basis0[1]);
			words[9] = FloatToHalfBits(basis1[1]);
			words[10] = FloatToHalfBits(basis2[1]);
			words[11] = FloatToHalfBits(basis2[2]);
			words[12] = FloatToHalfBits(basis0[2]);
			// The shader scales the model by 1 + this (in the GRAS record's scaled axes).
			words[13] = FloatToHalfBits(a_heightRandom * a_grass.heightRange);
			words[14] = 0;
			words[15] = 0;

			a_blade.position[0] = a_x;
			a_blade.position[1] = a_y;
			a_blade.position[2] = a_sample.height;
			a_blade.brightness = a_brightness;
			a_blade.orientation = a_orientation;
			a_blade.heightRandom = a_heightRandom;
		}

		void EncodeBladeScale(BladeCandidate& a_blade, const GameData::GrassInfo& a_grass, float a_globalScale)
		{
			// As NGIO's hook: the engine's final scale (1 + offset) times the global scale.
			a_blade.words[13] = FloatToHalfBits((1.0f + a_blade.heightRandom * a_grass.heightRange) * a_globalScale - 1.0f);
		}
	}

	void MoveBlade(BladeCandidate& a_blade, const CellCandidates& a_cell, float a_z, const float (&a_normal)[3], float a_globalScale)
	{
		const auto& grass = *a_cell.groups[a_blade.groupIndex].grass;
		Internal::TerrainSample surface;
		surface.height = a_z;
		surface.normal[0] = a_normal[0];
		surface.normal[1] = a_normal[1];
		surface.normal[2] = a_normal[2];
		Internal::EncodeBlade(a_blade, a_cell.cellX, a_cell.cellY, a_blade.position[0], a_blade.position[1], surface, grass, a_blade.brightness, a_blade.orientation,
			a_blade.heightRandom, true);
		if (a_globalScale != 1.0f) {
			Internal::EncodeBladeScale(a_blade, grass, a_globalScale);
		}
	}

	CellCandidates GenerateCellCandidates(const GameData::StaticWorldSnapshot& a_snapshot, const GameData::LandInfo& a_land, const PlacementSettings& a_settings)
	{
		if (!a_land.cellX || !a_land.cellY || !a_land.hasHeights) {
			return {};
		}
		auto cell = a_settings.mode == PlacementMode::Smooth ? Internal::GenerateSmoothCellCandidates(a_snapshot, a_land, a_settings)
		                                                     : Internal::GenerateVanillaCellCandidates(a_snapshot, a_land, a_settings);
		if (a_settings.globalScale != 1.0f) {
			for (auto& blade : cell.blades) {
				Internal::EncodeBladeScale(blade, *cell.groups[blade.groupIndex].grass, a_settings.globalScale);
			}
		}
		return cell;
	}
}
