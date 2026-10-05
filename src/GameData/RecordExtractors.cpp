#include "GameData/Internal/RecordExtractors.h"

#include <array>
#include <cmath>

namespace FasterNGIO::GameData::Internal
{
	namespace
	{
		[[nodiscard]] bool IsValidCellWaterHeight(float a_value) noexcept
		{
			return std::isfinite(a_value) && std::abs(a_value) < 100000000.0f;
		}

		[[nodiscard]] FormID ResolveDefaultLandTexture(FormID a_landTextureFormID)
		{
			return a_landTextureFormID.IsNull() ? kSkyrimDefaultLandTextureFormID : a_landTextureFormID;
		}

		[[nodiscard]] ObjectBounds ReadObjectBounds(std::span<const std::uint8_t> a_payload)
		{
			ObjectBounds bounds{};
			if (a_payload.size() >= 12) {
				for (std::size_t i = 0; i < 3; ++i) {
					bounds.min[i] = ReadLE<std::int16_t>(a_payload, i * 2);
					bounds.max[i] = ReadLE<std::int16_t>(a_payload, 6 + i * 2);
				}
				bounds.present = true;
			}
			return bounds;
		}

		// VHGT: a starting height, then one signed delta per vertex (each row starts from the
		// previous row's first vertex), in units of 8.
		[[nodiscard]] std::array<float, LandInfo::VertexCount> DecodeLandHeights(std::span<const std::uint8_t> a_vhgtPayload)
		{
			std::array<float, LandInfo::VertexCount> heights{};
			float rowBase = ReadLE<float>(a_vhgtPayload, 0);
			std::size_t gradientOffset = sizeof(float);
			for (std::size_t y = 0; y < LandInfo::VertexSide; ++y) {
				rowBase += static_cast<float>(static_cast<std::int8_t>(a_vhgtPayload[gradientOffset++]));
				float height = rowBase;
				heights[y * LandInfo::VertexSide] = height * 8.0f;
				for (std::size_t x = 1; x < LandInfo::VertexSide; ++x) {
					height += static_cast<float>(static_cast<std::int8_t>(a_vhgtPayload[gradientOffset++]));
					heights[y * LandInfo::VertexSide + x] = height * 8.0f;
				}
			}
			return heights;
		}

		// The CELL subrecords FasterNGIO reads.
		enum CellField : unsigned
		{
			kCellFlags = 1u << 0,
			kCellGrid = 1u << 1,
			kCellWater = 1u << 2,
			kCellAll = kCellFlags | kCellGrid | kCellWater
		};

		// Applies one CELL subrecord; returns the field it supplied (0 for none).
		unsigned ReadCellSubrecord(CellInfo& a_cell, FourCC a_signature, std::span<const std::uint8_t> a_payload)
		{
			if (a_signature == kSigData) {
				if (a_payload.size() == 1) {
					a_cell.cellFlags = a_payload[0];
					return kCellFlags;
				}
				if (a_payload.size() >= 2) {
					a_cell.cellFlags = ReadLE<std::uint16_t>(a_payload, 0);
					return kCellFlags;
				}
			} else if (a_signature == kSigXclc && a_payload.size() >= 8) {
				a_cell.gridX = ReadLE<std::int32_t>(a_payload, 0);
				a_cell.gridY = ReadLE<std::int32_t>(a_payload, 4);
				return kCellGrid;
			} else if (a_signature == kSigXclw && a_payload.size() >= 4) {
				if (const auto waterHeight = ReadLE<float>(a_payload, 0); IsValidCellWaterHeight(waterHeight)) {
					a_cell.waterHeight = waterHeight;
				}
				return kCellWater;
			}
			return 0;
		}
	}

	WorldInfo ExtractWorld(const RecordContext& a_record, std::span<const std::uint8_t> a_data)
	{
		WorldInfo info;
		info.formID = a_record.formID;
		SubrecordCursor cursor(a_data);
		Subrecord subrecord;
		while (cursor.Next(subrecord)) {
			if (subrecord.signature == kSigEdid) {
				info.editorID = ReadString(subrecord.payload);
				break;
			}
		}
		return info;
	}

	CellInfo ExtractCell(const RecordContext& a_record, std::span<const std::uint8_t> a_data)
	{
		CellInfo info;
		info.formID = a_record.formID;
		info.worldFormID = a_record.world;
		SubrecordCursor cursor(a_data);
		Subrecord subrecord;
		while (cursor.Next(subrecord)) {
			(void)ReadCellSubrecord(info, subrecord.signature, subrecord.payload);
		}
		return info;
	}

	CellInfo ExtractCompressedCell(const RecordContext& a_record, std::span<const std::uint8_t> a_compressedData, CellScanScratch& a_scratch)
	{
		if (a_compressedData.size() < sizeof(std::uint32_t)) {
			throw std::runtime_error("compressed CELL record is missing uncompressed size");
		}
		CellInfo info;
		info.formID = a_record.formID;
		info.worldFormID = a_record.world;
		auto& pending = a_scratch.pending;
		pending.clear();
		std::size_t consumed = 0;
		unsigned found = 0;
		a_scratch.stream.Reset(a_compressedData);

		std::array<std::uint8_t, 64> chunk{};
		while (found != kCellAll) {
			const auto produced = a_scratch.stream.Read(chunk);
			if (produced == 0) {
				break;
			}
			pending.insert(pending.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(produced));
			// Every whole subrecord inflated so far.
			while (found != kCellAll) {
				const std::span<const std::uint8_t> bytes(pending);
				const auto header = PeekSubrecordHeader(bytes, consumed);
				if (!header || consumed + header->headerSize + header->size > bytes.size()) {
					break;
				}
				found |= ReadCellSubrecord(info, header->signature, bytes.subspan(consumed + header->headerSize, header->size));
				consumed += header->headerSize + header->size;
			}
			if (consumed > 4096 && consumed * 2 >= pending.size()) {
				pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(consumed));
				consumed = 0;
			}
		}
		return info;
	}

	LandInfo ExtractLand(const RecordContext& a_record, std::span<const std::uint8_t> a_data)
	{
		LandInfo info{};
		info.formID = a_record.formID;
		info.parentCell = a_record.cell.value_or(FormID{});
		info.worldFormID = a_record.world;
		for (auto& color : info.vertexColors) {
			color = { 255, 255, 255 };
		}

		const auto& ids = a_record.localFormIDs;
		// VTXT belongs to the ATXT before it.
		std::optional<std::uint16_t> currentLayerIndex;
		std::optional<std::uint8_t> currentQuadrant;
		SubrecordCursor cursor(a_data);
		Subrecord subrecord;
		while (cursor.Next(subrecord)) {
			const auto payload = subrecord.payload;
			if (subrecord.signature == kSigVhgt && payload.size() >= sizeof(float) + LandInfo::VertexCount) {
				info.heights = DecodeLandHeights(payload);
				info.hasHeights = true;
			} else if (subrecord.signature == kSigVclr && payload.size() >= LandInfo::VertexCount * 3) {
				for (std::size_t i = 0; i < LandInfo::VertexCount; ++i) {
					info.vertexColors[i] = { payload[i * 3 + 0], payload[i * 3 + 1], payload[i * 3 + 2] };
				}
				info.hasVertexColors = true;
			} else if (subrecord.signature == kSigBtxt && payload.size() >= 8) {
				const auto quadrant = payload[4];
				if (quadrant < LandInfo::QuadrantCount) {
					info.baseTextures[quadrant] = LandBaseTexture{
						.landTextureFormID = ResolveDefaultLandTexture(ids.Resolve(FormID{ ReadLE<std::uint32_t>(payload, 0) })),
						.quadrant = quadrant,
					};
				}
				currentLayerIndex.reset();
				currentQuadrant.reset();
			} else if (subrecord.signature == kSigAtxt && payload.size() >= 8) {
				const auto quadrant = payload[4];
				const auto layerIndex = ReadLE<std::uint16_t>(payload, 6);
				info.alphaTextures.push_back(LandAlphaTexture{
					.landTextureFormID = ResolveDefaultLandTexture(ids.Resolve(FormID{ ReadLE<std::uint32_t>(payload, 0) })),
					.quadrant = quadrant,
					.layerIndex = layerIndex,
				});
				currentQuadrant = quadrant;
				currentLayerIndex = layerIndex;
			} else if (subrecord.signature == kSigVtxt && currentLayerIndex && currentQuadrant) {
				for (std::size_t offset = 0; offset + 8 <= payload.size(); offset += 8) {
					info.vertexAlphas.push_back(LandVertexAlpha{
						.quadrant = *currentQuadrant,
						.layerIndex = *currentLayerIndex,
						.position = ReadLE<std::uint16_t>(payload, offset + 0),
						.opacity = ReadLE<float>(payload, offset + 4),
					});
				}
			}
		}
		return info;
	}

	std::optional<PlacementInfo> ExtractPlacement(const RecordContext& a_record, std::span<const std::uint8_t> a_data)
	{
		PlacementInfo info{};
		info.formID = a_record.formID;
		info.parentCell = a_record.cell.value_or(FormID{});
		info.worldFormID = a_record.world;
		info.flags = a_record.header.flags;

		bool hasBase = false;
		SubrecordCursor cursor(a_data);
		Subrecord subrecord;
		while (cursor.Next(subrecord)) {
			const auto payload = subrecord.payload;
			if (subrecord.signature == kSigName && payload.size() >= 4) {
				info.baseFormID = a_record.localFormIDs.Resolve(FormID{ ReadLE<std::uint32_t>(payload, 0) });
				hasBase = true;
			} else if (subrecord.signature == kSigData && payload.size() >= 24) {
				for (std::size_t i = 0; i < 3; ++i) {
					info.position[i] = ReadLE<float>(payload, i * 4);
					info.rotation[i] = ReadLE<float>(payload, 12 + i * 4);
				}
			} else if (subrecord.signature == kSigXscl && payload.size() >= 4) {
				info.scale = ReadLE<float>(payload, 0);
			}
		}
		if (!hasBase) {
			return std::nullopt;
		}
		return info;
	}

	BaseObjectInfo ExtractBaseObject(const RecordContext& a_record, std::span<const std::uint8_t> a_data)
	{
		BaseObjectInfo info;
		info.formID = a_record.formID;
		SubrecordCursor cursor(a_data);
		Subrecord subrecord;
		while (cursor.Next(subrecord)) {
			if (subrecord.signature == kSigModl) {
				info.modelPath = ReadString(subrecord.payload);
			} else if (subrecord.signature == kSigObnd) {
				info.bounds = ReadObjectBounds(subrecord.payload);
			}
		}
		return info;
	}

	LandTextureInfo ExtractLandTexture(const RecordContext& a_record, std::span<const std::uint8_t> a_data)
	{
		LandTextureInfo info;
		info.formID = a_record.formID;
		SubrecordCursor cursor(a_data);
		Subrecord subrecord;
		while (cursor.Next(subrecord)) {
			if (subrecord.signature == kSigGnam && subrecord.payload.size() >= 4) {
				info.grassFormIDs.push_back(a_record.localFormIDs.Resolve(FormID{ ReadLE<std::uint32_t>(subrecord.payload, 0) }));
			}
		}
		return info;
	}

	GrassInfo ExtractGrass(const RecordContext& a_record, std::span<const std::uint8_t> a_data)
	{
		GrassInfo info;
		info.formID = a_record.formID;
		SubrecordCursor cursor(a_data);
		Subrecord subrecord;
		while (cursor.Next(subrecord)) {
			const auto payload = subrecord.payload;
			if (subrecord.signature == kSigModl) {
				info.modelPath = ReadString(payload);
			} else if (subrecord.signature == kSigObnd) {
				info.bounds = ReadObjectBounds(payload);
			} else if (subrecord.signature == kSigData && payload.size() >= 0x20) {
				info.density = ReadLE<std::uint8_t>(payload, 0);
				info.minSlopeDegrees = ReadLE<std::uint8_t>(payload, 1);
				info.maxSlopeDegrees = ReadLE<std::uint8_t>(payload, 2);
				info.distanceFromWaterLevel = ReadLE<std::uint16_t>(payload, 4);
				info.underwaterState = static_cast<GrassWaterState>(ReadLE<std::uint32_t>(payload, 8));
				info.positionRange = ReadLE<float>(payload, 0x0C);
				info.heightRange = ReadLE<float>(payload, 0x10);
				info.colorRange = ReadLE<float>(payload, 0x14);
				info.wavePeriod = ReadLE<float>(payload, 0x18);
				info.grassFlags = ReadLE<std::uint8_t>(payload, 0x1C);
			}
		}
		return info;
	}
}
