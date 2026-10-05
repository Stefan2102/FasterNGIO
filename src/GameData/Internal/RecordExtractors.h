#pragma once

#include "GameData/Internal/RecordReader.h"
#include "GameData/Records.h"

#include <optional>
#include <span>
#include <vector>

// Typed records from their (uncompressed) subrecord data.
namespace FasterNGIO::GameData::Internal
{
	// Where a record sits: its header, its load-order form ID and the world and cell groups it is in.
	struct RecordContext
	{
		const RecordHeader& header;
		FormID formID;
		std::optional<FormID> world;
		std::optional<FormID> cell;
		const LocalFormIDs& localFormIDs;
	};

	[[nodiscard]] WorldInfo ExtractWorld(const RecordContext& a_record, std::span<const std::uint8_t> a_data);
	[[nodiscard]] CellInfo ExtractCell(const RecordContext& a_record, std::span<const std::uint8_t> a_data);
	[[nodiscard]] LandInfo ExtractLand(const RecordContext& a_record, std::span<const std::uint8_t> a_data);
	// Nullopt for a reference without a base object.
	[[nodiscard]] std::optional<PlacementInfo> ExtractPlacement(const RecordContext& a_record, std::span<const std::uint8_t> a_data);
	[[nodiscard]] BaseObjectInfo ExtractBaseObject(const RecordContext& a_record, std::span<const std::uint8_t> a_data);
	[[nodiscard]] LandTextureInfo ExtractLandTexture(const RecordContext& a_record, std::span<const std::uint8_t> a_data);
	[[nodiscard]] GrassInfo ExtractGrass(const RecordContext& a_record, std::span<const std::uint8_t> a_data);

	// Buffers reused across compressed CELL records.
	struct CellScanScratch
	{
		std::vector<std::uint8_t> pending;
		InflateStream stream;
	};

	// A compressed CELL, inflated only as far as its DATA, XCLC and XCLW: cells are numerous and
	// those come first.
	[[nodiscard]] CellInfo ExtractCompressedCell(const RecordContext& a_record, std::span<const std::uint8_t> a_compressedData, CellScanScratch& a_scratch);
}
