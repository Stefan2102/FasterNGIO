#include "GameData/Internal/RecordExtractors.h"
#include "GameData/Internal/RecordReader.h"
#include "GameData/StaticWorld.h"
#include "Platform/MappedFile.h"

#include <stdexcept>

namespace FasterNGIO::GameData
{
	namespace
	{
		using namespace Internal;

		struct ParserContext
		{
			StaticPluginShard& shard;
			LocalFormIDs localFormIDs;
			// The WRLD and CELL whose groups the parser is in.
			std::optional<FormID> currentWorld{};
			std::optional<FormID> currentCell{};
			std::vector<std::uint8_t> inflateScratch{};
			CellScanScratch cellScratch{};
		};

		// Group types whose label is a parent record: world children (1) and the cell children,
		// persistent, temporary and visible-distant groups (6, 8, 9, 10).
		void EnterGroup(ParserContext& a_context, const GroupHeader& a_group)
		{
			if (a_group.type != 1 && a_group.type != 6 && a_group.type != 8 && a_group.type != 9 && a_group.type != 10) {
				return;
			}
			const auto parent = a_context.localFormIDs.Resolve(FormID{ a_group.label });
			if (a_group.type == 1) {
				a_context.currentWorld = parent;
			} else {
				a_context.currentCell = parent;
			}
		}

		[[nodiscard]] bool IsParsedSignature(FourCC a_signature)
		{
			return a_signature == kSigWrld || a_signature == kSigCell || a_signature == kSigLand || a_signature == kSigRefr || a_signature == kSigAchr ||
			       a_signature == kSigLtex || a_signature == kSigGras || IsBaseObjectSignature(a_signature);
		}

		void ParseRecord(std::span<const std::uint8_t> a_bytes, std::size_t a_offset, ParserContext& a_context)
		{
			const auto header = ReadRecordHeader(a_bytes, a_offset);
			if (!IsParsedSignature(header.signature)) {
				return;
			}

			auto& shard = a_context.shard;
			const auto formID = a_context.localFormIDs.Resolve(header.formID);
			if (header.IsSuppressed()) {
				if (!formID.IsEmpty()) {
					shard.suppressors.push_back(StaticRecordSuppressor{ .formID = formID, .signature = header.signature });
				}
				if (header.signature == kSigWrld) {
					a_context.currentWorld = formID;
				} else if (header.signature == kSigCell) {
					a_context.currentCell = formID;
				}
				return;
			}

			const RecordContext record{
				.header = header,
				.formID = formID,
				.world = a_context.currentWorld,
				.cell = a_context.currentCell,
				.localFormIDs = a_context.localFormIDs,
			};
			std::span<const std::uint8_t> data = a_bytes.subspan(a_offset + kRecordHeaderSize, header.dataSize);
			if (header.IsCompressed()) {
				if (header.signature == kSigCell) {
					shard.cells.push_back(ExtractCompressedCell(record, data, a_context.cellScratch));
					a_context.currentCell = formID;
					return;
				}
				InflateRecord(data, a_context.inflateScratch);
				data = a_context.inflateScratch;
			}

			if (header.signature == kSigWrld) {
				shard.worlds.push_back(ExtractWorld(record, data));
				a_context.currentWorld = formID;
			} else if (header.signature == kSigCell) {
				shard.cells.push_back(ExtractCell(record, data));
				a_context.currentCell = formID;
			} else if (header.signature == kSigLand) {
				shard.lands.push_back(ExtractLand(record, data));
			} else if (header.signature == kSigRefr || header.signature == kSigAchr) {
				if (auto placement = ExtractPlacement(record, data)) {
					shard.placements.push_back(std::move(*placement));
				}
			} else if (header.signature == kSigLtex) {
				shard.landTextures.push_back(ExtractLandTexture(record, data));
			} else if (header.signature == kSigGras) {
				shard.grasses.push_back(ExtractGrass(record, data));
			} else {
				shard.baseObjects.push_back(ExtractBaseObject(record, data));
			}
		}

		void ParseRange(std::span<const std::uint8_t> a_bytes, std::size_t a_begin, std::size_t a_end, ParserContext& a_context)
		{
			std::size_t offset = a_begin;
			while (offset + kRecordHeaderSize <= a_end) {
				if (ReadLE<FourCC>(a_bytes, offset) == kSigGrup) {
					const auto group = ReadGroupHeader(a_bytes, offset);
					if (group.groupSize < kRecordHeaderSize || offset + group.groupSize > a_end) {
						throw std::runtime_error("invalid GRUP size in " + a_context.shard.entry.pluginName);
					}
					const auto savedWorld = a_context.currentWorld;
					const auto savedCell = a_context.currentCell;
					EnterGroup(a_context, group);
					ParseRange(a_bytes, offset + kRecordHeaderSize, offset + group.groupSize, a_context);
					a_context.currentWorld = savedWorld;
					a_context.currentCell = savedCell;
					offset += group.groupSize;
				} else {
					const auto header = ReadRecordHeader(a_bytes, offset);
					if (offset + kRecordHeaderSize + header.dataSize > a_end) {
						throw std::runtime_error("invalid record size in " + a_context.shard.entry.pluginName);
					}
					ParseRecord(a_bytes, offset, a_context);
					offset += kRecordHeaderSize + header.dataSize;
				}
			}
		}
	}

	StaticPluginShard ParseStaticWorldShard(const LoadOrderEntry& a_entry, std::span<const LoadOrderEntry> a_loadOrder)
	{
		StaticPluginShard shard;
		shard.entry = a_entry;
		const Platform::MappedFile file(a_entry.path, Platform::MappedFile::Access::Sequential);
		const auto bytes = file.Bytes();
		const auto tes4 = ReadTes4Header(bytes, a_entry.path);

		const auto reserveCount = static_cast<std::size_t>(a_entry.recordCount);
		shard.baseObjects.reserve(reserveCount / 12);
		shard.cells.reserve(reserveCount / 8);
		shard.placements.reserve(reserveCount / 2);
		shard.suppressors.reserve(reserveCount / 32);

		ParserContext context{ .shard = shard, .localFormIDs = LocalFormIDs(shard.entry, a_loadOrder) };
		ParseRange(bytes, kRecordHeaderSize + tes4.dataSize, bytes.size(), context);
		return shard;
	}
}
