#include "GameData/GameData.h"
#include "GameData/Internal/PluginFileReader.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/enumerable_thread_specific.h>
#include <oneapi/tbb/parallel_for.h>
#include <zlib.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace FasterNGIO::GameData
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr FourCC SIG_TES4 = MakeFourCC('T', 'E', 'S', '4');
		constexpr FourCC SIG_GRUP = MakeFourCC('G', 'R', 'U', 'P');
		constexpr FourCC SIG_XXXX = MakeFourCC('X', 'X', 'X', 'X');
		constexpr FourCC SIG_EDID = MakeFourCC('E', 'D', 'I', 'D');
		constexpr FourCC SIG_MAST = MakeFourCC('M', 'A', 'S', 'T');
		constexpr FourCC SIG_CNAM = MakeFourCC('C', 'N', 'A', 'M');
		constexpr FourCC SIG_SNAM = MakeFourCC('S', 'N', 'A', 'M');
		constexpr FourCC SIG_HEDR = MakeFourCC('H', 'E', 'D', 'R');
		constexpr FourCC SIG_ONAM = MakeFourCC('O', 'N', 'A', 'M');
		constexpr FourCC SIG_MODL = MakeFourCC('M', 'O', 'D', 'L');
		constexpr FourCC SIG_MOD2 = MakeFourCC('M', 'O', 'D', '2');
		constexpr FourCC SIG_MOD3 = MakeFourCC('M', 'O', 'D', '3');
		constexpr FourCC SIG_MOD4 = MakeFourCC('M', 'O', 'D', '4');
		constexpr FourCC SIG_MOD5 = MakeFourCC('M', 'O', 'D', '5');
		constexpr FourCC SIG_MODS = MakeFourCC('M', 'O', 'D', 'S');
		constexpr FourCC SIG_MO2S = MakeFourCC('M', 'O', '2', 'S');
		constexpr FourCC SIG_MO3S = MakeFourCC('M', 'O', '3', 'S');
		constexpr FourCC SIG_MO4S = MakeFourCC('M', 'O', '4', 'S');
		constexpr FourCC SIG_MO5S = MakeFourCC('M', 'O', '5', 'S');
		constexpr FourCC SIG_MNAM = MakeFourCC('M', 'N', 'A', 'M');
		constexpr FourCC SIG_NAME = MakeFourCC('N', 'A', 'M', 'E');
		constexpr FourCC SIG_DATA = MakeFourCC('D', 'A', 'T', 'A');
		constexpr FourCC SIG_XCLC = MakeFourCC('X', 'C', 'L', 'C');
		constexpr FourCC SIG_XCLW = MakeFourCC('X', 'C', 'L', 'W');
		constexpr FourCC SIG_XSCL = MakeFourCC('X', 'S', 'C', 'L');
		constexpr FourCC SIG_XESP = MakeFourCC('X', 'E', 'S', 'P');
		constexpr FourCC SIG_STAT = MakeFourCC('S', 'T', 'A', 'T');
		constexpr FourCC SIG_MSTT = MakeFourCC('M', 'S', 'T', 'T');
		constexpr FourCC SIG_TREE = MakeFourCC('T', 'R', 'E', 'E');
		constexpr FourCC SIG_ACTI = MakeFourCC('A', 'C', 'T', 'I');
		constexpr FourCC SIG_DOOR = MakeFourCC('D', 'O', 'O', 'R');
		constexpr FourCC SIG_CONT = MakeFourCC('C', 'O', 'N', 'T');
		constexpr FourCC SIG_FURN = MakeFourCC('F', 'U', 'R', 'N');
		constexpr FourCC SIG_LIGH = MakeFourCC('L', 'I', 'G', 'H');
		constexpr FourCC SIG_FLOR = MakeFourCC('F', 'L', 'O', 'R');
		constexpr FourCC SIG_SCOL = MakeFourCC('S', 'C', 'O', 'L');
		constexpr FourCC SIG_TACT = MakeFourCC('T', 'A', 'C', 'T');
		constexpr FourCC SIG_OBND = MakeFourCC('O', 'B', 'N', 'D');
		constexpr FourCC SIG_WRLD = MakeFourCC('W', 'R', 'L', 'D');
		constexpr FourCC SIG_CELL = MakeFourCC('C', 'E', 'L', 'L');
		constexpr FourCC SIG_REFR = MakeFourCC('R', 'E', 'F', 'R');
		constexpr FourCC SIG_ACHR = MakeFourCC('A', 'C', 'H', 'R');
		constexpr FourCC SIG_TXST = MakeFourCC('T', 'X', 'S', 'T');
		constexpr FourCC SIG_LTEX = MakeFourCC('L', 'T', 'E', 'X');
		constexpr FourCC SIG_GRAS = MakeFourCC('G', 'R', 'A', 'S');
		constexpr FourCC SIG_LAND = MakeFourCC('L', 'A', 'N', 'D');
		constexpr FourCC SIG_TX00 = MakeFourCC('T', 'X', '0', '0');
		constexpr FourCC SIG_TX01 = MakeFourCC('T', 'X', '0', '1');
		constexpr FourCC SIG_TX02 = MakeFourCC('T', 'X', '0', '2');
		constexpr FourCC SIG_TX03 = MakeFourCC('T', 'X', '0', '3');
		constexpr FourCC SIG_TX04 = MakeFourCC('T', 'X', '0', '4');
		constexpr FourCC SIG_TX05 = MakeFourCC('T', 'X', '0', '5');
		constexpr FourCC SIG_TX06 = MakeFourCC('T', 'X', '0', '6');
		constexpr FourCC SIG_TX07 = MakeFourCC('T', 'X', '0', '7');
		constexpr FourCC SIG_DNAM = MakeFourCC('D', 'N', 'A', 'M');
		constexpr FourCC SIG_HNAM = MakeFourCC('H', 'N', 'A', 'M');
		constexpr FourCC SIG_GNAM = MakeFourCC('G', 'N', 'A', 'M');
		constexpr FourCC SIG_TNAM = MakeFourCC('T', 'N', 'A', 'M');
		constexpr FourCC SIG_INAM = MakeFourCC('I', 'N', 'A', 'M');
		constexpr FourCC SIG_VHGT = MakeFourCC('V', 'H', 'G', 'T');
		constexpr FourCC SIG_VNML = MakeFourCC('V', 'N', 'M', 'L');
		constexpr FourCC SIG_VCLR = MakeFourCC('V', 'C', 'L', 'R');
		constexpr FourCC SIG_BTXT = MakeFourCC('B', 'T', 'X', 'T');
		constexpr FourCC SIG_ATXT = MakeFourCC('A', 'T', 'X', 'T');
		constexpr FourCC SIG_VTXT = MakeFourCC('V', 'T', 'X', 'T');

		constexpr std::uint32_t RECORD_FLAG_COMPRESSED = 0x00040000u;
		constexpr std::uint32_t TES4_FLAG_ESL = 0x00000200u;

		template <class T>
		[[nodiscard]] T ReadLE(std::span<const std::uint8_t> a_bytes, std::size_t a_offset)
		{
			if (a_offset + sizeof(T) > a_bytes.size()) {
				throw std::runtime_error("unexpected end of plugin data");
			}
			T value{};
			std::memcpy(std::addressof(value), a_bytes.data() + a_offset, sizeof(T));
			return value;
		}

		[[nodiscard]] bool IsValidCellWaterHeight(float a_value) noexcept
		{
			return std::isfinite(a_value) && std::abs(a_value) < 100000000.0f;
		}


		using Internal::MapFileBytes;
		using Internal::ReadFileBytes;

		struct HeaderCacheEntry
		{
			std::filesystem::file_time_type writeTime{};
			std::uintmax_t fileSize{ 0 };
			PluginHeader header;
		};

		std::mutex g_headerCacheMutex;
		std::unordered_map<std::wstring, HeaderCacheEntry> g_headerCache;

		[[nodiscard]] std::string ReadString(std::span<const std::uint8_t> a_data)
		{
			const auto* begin = reinterpret_cast<const char*>(a_data.data());
			const auto* end = begin + a_data.size();
			const auto* nullPos = std::find(begin, end, '\0');
			return std::string(begin, nullPos);
		}

		[[nodiscard]] FormID ResolveDefaultLandTexture(FormID a_landTextureFormID)
		{
			return a_landTextureFormID.IsNull() ? kSkyrimDefaultLandTextureFormID : a_landTextureFormID;
		}

		[[nodiscard]] std::string ReadString260(std::span<const std::uint8_t> a_data, std::size_t a_index)
		{
			const auto offset = a_index * 260u;
			if (offset >= a_data.size()) {
				return {};
			}
			const auto size = (std::min<std::size_t>)(260u, a_data.size() - offset);
			return ReadString(a_data.subspan(offset, size));
		}

		[[nodiscard]] bool IsModelPathSubrecord(FourCC a_signature)
		{
			return a_signature == SIG_MODL ||
			       a_signature == SIG_MOD2 ||
			       a_signature == SIG_MOD3 ||
			       a_signature == SIG_MOD4 ||
			       a_signature == SIG_MOD5;
		}

		[[nodiscard]] bool IsModelTextureSwapSubrecord(FourCC a_signature)
		{
			return a_signature == SIG_MODS ||
			       a_signature == SIG_MO2S ||
			       a_signature == SIG_MO3S ||
			       a_signature == SIG_MO4S ||
			       a_signature == SIG_MO5S;
		}

		[[nodiscard]] std::optional<std::size_t> TextureSlotIndexForSubrecord(FourCC a_signature)
		{
			switch (a_signature) {
			case SIG_TX00:
				return 0;
			case SIG_TX01:
				return 1;
			case SIG_TX02:
				return 2;
			case SIG_TX03:
				return 3;
			case SIG_TX04:
				return 4;
			case SIG_TX05:
				return 5;
			case SIG_TX06:
				return 6;
			case SIG_TX07:
				return 7;
			default:
				return std::nullopt;
			}
		}

		[[nodiscard]] std::vector<std::uint8_t> InflateRecord(std::span<const std::uint8_t> a_recordData)
		{
			if (a_recordData.size() < sizeof(std::uint32_t)) {
				throw std::runtime_error("compressed record is missing uncompressed size");
			}
			const auto uncompressedSize = ReadLE<std::uint32_t>(a_recordData, 0);
			std::vector<std::uint8_t> output(uncompressedSize);
			uLongf outputSize = static_cast<uLongf>(output.size());
			const auto* compressed = reinterpret_cast<const Bytef*>(a_recordData.data() + sizeof(std::uint32_t));
			const auto compressedSize = static_cast<uLong>(a_recordData.size() - sizeof(std::uint32_t));
			const auto result = uncompress(reinterpret_cast<Bytef*>(output.data()), std::addressof(outputSize), compressed, compressedSize);
			if (result != Z_OK || outputSize != output.size()) {
				throw std::runtime_error("zlib failed to inflate compressed record");
			}
			return output;
		}

		void InflateRecordInto(std::span<const std::uint8_t> a_recordData, std::vector<std::uint8_t>& a_output)
		{
			if (a_recordData.size() < sizeof(std::uint32_t)) {
				throw std::runtime_error("compressed record is missing uncompressed size");
			}
			const auto uncompressedSize = ReadLE<std::uint32_t>(a_recordData, 0);
			a_output.resize(uncompressedSize);
			uLongf outputSize = static_cast<uLongf>(a_output.size());
			const auto* compressed = reinterpret_cast<const Bytef*>(a_recordData.data() + sizeof(std::uint32_t));
			const auto compressedSize = static_cast<uLong>(a_recordData.size() - sizeof(std::uint32_t));
			const auto result = uncompress(reinterpret_cast<Bytef*>(a_output.data()), std::addressof(outputSize), compressed, compressedSize);
			if (result != Z_OK || outputSize != a_output.size()) {
				throw std::runtime_error("zlib failed to inflate compressed record");
			}
		}

		[[nodiscard]] double ElapsedMilliseconds(Clock::time_point a_begin)
		{
			return std::chrono::duration<double, std::milli>(Clock::now() - a_begin).count();
		}

		[[nodiscard]] bool IsPluginExtension(std::string_view a_name)
		{
			const auto lower = LowerAscii(a_name);
			return lower.ends_with(".esm") || lower.ends_with(".esp") || lower.ends_with(".esl");
		}

		[[nodiscard]] std::string PluginNameFromPath(const std::filesystem::path& a_path)
		{
			return LowerAscii(a_path.filename().string());
		}

		[[nodiscard]] std::size_t FindLoadOrderIndex(std::span<const LoadOrderEntry> a_loadOrder, std::string_view a_pluginName)
		{
			const auto key = LowerAscii(a_pluginName);
			for (std::size_t i = 0; i < a_loadOrder.size(); ++i) {
				if (LowerAscii(a_loadOrder[i].pluginName) == key) {
					return i;
				}
			}
			throw std::runtime_error("missing master in load order: " + std::string(a_pluginName));
		}

		[[nodiscard]] std::shared_ptr<const std::vector<FileID>> BuildSourceMasterFileIDs(const LoadOrderEntry& a_entry, std::span<const LoadOrderEntry> a_loadOrder)
		{
			std::vector<FileID> result;
			result.reserve(a_entry.masters.size());
			for (const auto& master : a_entry.masters) {
				result.push_back(a_loadOrder[FindLoadOrderIndex(a_loadOrder, master)].fileID);
			}
			return std::make_shared<const std::vector<FileID>>(std::move(result));
		}

		[[nodiscard]] FormID ConvertRecordLocalFormID(FormID a_fileFormID, const RawRecord& a_record)
		{
			if (a_fileFormID.IsNull() || a_fileFormID.IsNone() || a_fileFormID.IsHardcoded()) {
				return a_fileFormID;
			}

			const auto rawFileSlot = static_cast<std::uint8_t>(a_fileFormID.value >> 24);
			auto owner = a_record.sourceFileID;
			if (a_record.sourceMasterFileIDs && rawFileSlot < a_record.sourceMasterFileIDs->size()) {
				owner = (*a_record.sourceMasterFileIDs)[rawFileSlot];
			}

			const auto objectID = owner.kind == ModuleKind::Light ? (a_fileFormID.value & 0xFFFu) : (a_fileFormID.value & 0xFFFFFFu);
			return FormID{ owner.BaseFormID() | objectID };
		}

		[[nodiscard]] FormID ConvertContextLocalFormID(
			FormID a_fileFormID,
			FileID a_sourceFileID,
			const std::shared_ptr<const std::vector<FileID>>& a_sourceMasterFileIDs)
		{
			if (a_fileFormID.IsNull() || a_fileFormID.IsNone() || a_fileFormID.IsHardcoded()) {
				return a_fileFormID;
			}

			const auto rawFileSlot = static_cast<std::uint8_t>(a_fileFormID.value >> 24);
			auto owner = a_sourceFileID;
			if (a_sourceMasterFileIDs && rawFileSlot < a_sourceMasterFileIDs->size()) {
				owner = (*a_sourceMasterFileIDs)[rawFileSlot];
			}

			const auto objectID = owner.kind == ModuleKind::Light ? (a_fileFormID.value & 0xFFFu) : (a_fileFormID.value & 0xFFFFFFu);
			return FormID{ owner.BaseFormID() | objectID };
		}

		struct RecordHeader
		{
			FourCC signature{ 0 };
			std::uint32_t dataSize{ 0 };
			std::uint32_t flags{ 0 };
			FormID formID{};
			std::uint32_t revision{ 0 };
			std::uint16_t formVersion{ 0 };
			std::uint16_t unknown{ 0 };
		};

		[[nodiscard]] RecordHeader ReadRecordHeader(std::span<const std::uint8_t> a_bytes, std::size_t a_offset)
		{
			RecordHeader header{};
			header.signature = ReadLE<FourCC>(a_bytes, a_offset + 0);
			header.dataSize = ReadLE<std::uint32_t>(a_bytes, a_offset + 4);
			header.flags = ReadLE<std::uint32_t>(a_bytes, a_offset + 8);
			header.formID = FormID{ ReadLE<std::uint32_t>(a_bytes, a_offset + 12) };
			header.revision = ReadLE<std::uint32_t>(a_bytes, a_offset + 16);
			header.formVersion = ReadLE<std::uint16_t>(a_bytes, a_offset + 20);
			header.unknown = ReadLE<std::uint16_t>(a_bytes, a_offset + 22);
			return header;
		}

		struct GroupHeader
		{
			std::uint32_t groupSize{ 0 };
			std::uint32_t label{ 0 };
			std::int32_t type{ 0 };
			std::int16_t gridY{ 0 };
			std::int16_t gridX{ 0 };
		};

		[[nodiscard]] GroupHeader ReadGroupHeader(std::span<const std::uint8_t> a_bytes, std::size_t a_offset)
		{
			GroupHeader header{};
			header.groupSize = ReadLE<std::uint32_t>(a_bytes, a_offset + 4);
			header.label = ReadLE<std::uint32_t>(a_bytes, a_offset + 8);
			header.type = ReadLE<std::int32_t>(a_bytes, a_offset + 12);
			header.gridY = ReadLE<std::int16_t>(a_bytes, a_offset + 8);
			header.gridX = ReadLE<std::int16_t>(a_bytes, a_offset + 10);
			return header;
		}

		[[nodiscard]] BaseObjectKind BaseKindForSignature(FourCC a_signature);
		[[nodiscard]] bool IsBaseObjectSignature(FourCC a_signature);

		[[nodiscard]] bool IsStaticWorldRecordSignature(FourCC a_signature)
		{
			return IsBaseObjectSignature(a_signature) ||
			       a_signature == SIG_TXST ||
			       a_signature == SIG_LTEX ||
			       a_signature == SIG_GRAS ||
			       a_signature == SIG_CELL ||
			       a_signature == SIG_LAND ||
			       a_signature == SIG_REFR ||
			       a_signature == SIG_ACHR;
		}

		[[nodiscard]] bool ShouldRetainRecord(FourCC a_recordSignature, PluginParser::ParseMode a_mode)
		{
			return a_mode == PluginParser::ParseMode::FullRaw || IsStaticWorldRecordSignature(a_recordSignature);
		}

		[[nodiscard]] bool ShouldRetainSubrecord(FourCC a_recordSignature, FourCC a_subrecordSignature, PluginParser::ParseMode a_mode)
		{
			if (a_mode == PluginParser::ParseMode::FullRaw || a_subrecordSignature == SIG_XXXX) {
				return true;
			}
			if (IsBaseObjectSignature(a_recordSignature)) {
				return a_subrecordSignature == SIG_EDID ||
				       a_subrecordSignature == SIG_MODL ||
				       a_subrecordSignature == SIG_OBND ||
				       a_subrecordSignature == SIG_MNAM ||
				       IsModelTextureSwapSubrecord(a_subrecordSignature);
			}
			if (a_recordSignature == SIG_TXST) {
				return a_subrecordSignature == SIG_EDID ||
				       a_subrecordSignature == SIG_DNAM ||
				       TextureSlotIndexForSubrecord(a_subrecordSignature).has_value();
			}
			if (a_recordSignature == SIG_LTEX) {
				return a_subrecordSignature == SIG_EDID ||
				       a_subrecordSignature == SIG_HNAM ||
				       a_subrecordSignature == SIG_SNAM ||
				       a_subrecordSignature == SIG_GNAM ||
				       a_subrecordSignature == SIG_TNAM ||
				       a_subrecordSignature == SIG_MNAM ||
				       a_subrecordSignature == SIG_INAM;
			}
			if (a_recordSignature == SIG_CELL) {
				return a_subrecordSignature == SIG_EDID || a_subrecordSignature == SIG_DATA || a_subrecordSignature == SIG_XCLC ||
				       a_subrecordSignature == SIG_XCLW;
			}
			if (a_recordSignature == SIG_LAND) {
				return a_subrecordSignature == SIG_VHGT ||
				       a_subrecordSignature == SIG_VNML ||
				       a_subrecordSignature == SIG_VCLR ||
				       a_subrecordSignature == SIG_BTXT ||
				       a_subrecordSignature == SIG_ATXT ||
				       a_subrecordSignature == SIG_VTXT;
			}
			if (a_recordSignature == SIG_REFR || a_recordSignature == SIG_ACHR) {
				return a_subrecordSignature == SIG_NAME || a_subrecordSignature == SIG_DATA || a_subrecordSignature == SIG_XSCL || a_subrecordSignature == SIG_XESP;
			}
			return false;
		}

		[[nodiscard]] std::vector<RawSubrecord> ParseSubrecords(
			std::span<const std::uint8_t> a_data,
			FourCC a_recordSignature = 0,
			PluginParser::ParseMode a_mode = PluginParser::ParseMode::FullRaw)
		{
			std::vector<RawSubrecord> subrecords;
			subrecords.reserve(8);
			std::size_t offset = 0;
			while (offset + 6 <= a_data.size()) {
				const auto signature = ReadLE<FourCC>(a_data, offset);
				const auto size16 = ReadLE<std::uint16_t>(a_data, offset + 4);
				offset += 6;

				std::uint32_t size = size16;
				if (signature == SIG_XXXX) {
					if (offset + 4 > a_data.size()) {
						break;
					}
					RawSubrecord xxxx{};
					xxxx.signature = signature;
					xxxx.size = 4;
					xxxx.dataOffset = static_cast<std::uint32_t>(offset);
					if (ShouldRetainSubrecord(a_recordSignature, xxxx.signature, a_mode)) {
						subrecords.push_back(std::move(xxxx));
					}
					size = ReadLE<std::uint32_t>(a_data, offset);
					offset += 4;
					if (offset + 6 > a_data.size()) {
						break;
					}
				}

				RawSubrecord subrecord{};
				subrecord.signature = signature == SIG_XXXX ? ReadLE<FourCC>(a_data, offset) : signature;
				if (signature == SIG_XXXX) {
					(void)ReadLE<std::uint16_t>(a_data, offset + 4);
					offset += 6;
				}
				subrecord.size = size;
				subrecord.dataOffset = static_cast<std::uint32_t>(offset);
				if (offset + size > a_data.size()) {
					break;
				}
				if (ShouldRetainSubrecord(a_recordSignature, subrecord.signature, a_mode)) {
					subrecords.push_back(std::move(subrecord));
				}
				offset += size;
			}
			return subrecords;
		}

		[[nodiscard]] std::span<const std::uint8_t> SubrecordData(std::span<const std::uint8_t> a_recordData, const RawSubrecord& a_subrecord)
		{
			if (a_subrecord.dataOffset > a_recordData.size() || a_subrecord.size > a_recordData.size() - a_subrecord.dataOffset) {
				return {};
			}
			return a_recordData.subspan(a_subrecord.dataOffset, a_subrecord.size);
		}

		[[nodiscard]] std::span<const std::uint8_t> SubrecordData(const RawRecord& a_record, const RawSubrecord& a_subrecord)
		{
			return SubrecordData(a_record.Data(), a_subrecord);
		}

		[[nodiscard]] const RawSubrecord* FindSubrecord(const RawRecord& a_record, FourCC a_signature)
		{
			for (const auto& subrecord : a_record.subrecords) {
				if (subrecord.signature == a_signature) {
					return std::addressof(subrecord);
				}
			}
			return nullptr;
		}

		class SubrecordCursor
		{
		public:
			explicit SubrecordCursor(std::span<const std::uint8_t> a_data) :
				_data(a_data)
			{}

			[[nodiscard]] bool Next(RawSubrecord& a_subrecord, std::span<const std::uint8_t>& a_payload)
			{
				if (_offset + 6 > _data.size()) {
					return false;
				}

				auto signature = ReadLE<FourCC>(_data, _offset);
				std::uint32_t size = ReadLE<std::uint16_t>(_data, _offset + 4);
				_offset += 6;

				if (signature == SIG_XXXX) {
					if (_offset + 4 > _data.size()) {
						_offset = _data.size();
						return false;
					}
					size = ReadLE<std::uint32_t>(_data, _offset);
					_offset += 4;
					if (_offset + 6 > _data.size()) {
						_offset = _data.size();
						return false;
					}
					signature = ReadLE<FourCC>(_data, _offset);
					_offset += 6;
				}

				if (_offset + size > _data.size()) {
					_offset = _data.size();
					return false;
				}

				a_subrecord.signature = signature;
				a_subrecord.size = size;
				a_subrecord.dataOffset = static_cast<std::uint32_t>(_offset);
				a_payload = _data.subspan(_offset, size);
				_offset += size;
				return true;
			}

		private:
			std::span<const std::uint8_t> _data;
			std::size_t _offset{ 0 };
		};

		void ParseHeaderRecordData(std::span<const std::uint8_t> a_data, PluginHeader& a_header)
		{
			const auto subrecords = ParseSubrecords(a_data);
			for (std::size_t i = 0; i < subrecords.size(); ++i) {
				const auto& subrecord = subrecords[i];
				const auto data = SubrecordData(a_data, subrecord);
				if (subrecord.signature == SIG_HEDR && data.size() >= 12) {
					a_header.version = ReadLE<float>(data, 0);
					a_header.recordCount = ReadLE<std::uint32_t>(data, 4);
					a_header.nextObjectID = ReadLE<std::uint32_t>(data, 8);
				} else if (subrecord.signature == SIG_CNAM) {
					a_header.author = ReadString(data);
				} else if (subrecord.signature == SIG_SNAM) {
					a_header.description = ReadString(data);
				} else if (subrecord.signature == SIG_MAST) {
					a_header.masters.push_back(ReadString(data));
				} else if (subrecord.signature == SIG_ONAM) {
					for (std::size_t offset = 0; offset + 4 <= data.size(); offset += 4) {
						a_header.onamOverrides.push_back(FormID{ ReadLE<std::uint32_t>(data, offset) });
					}
				}
			}
		}

		[[nodiscard]] ModuleKind DetectModuleKind(const std::filesystem::path& a_path, const PluginHeader& a_header)
		{
			const auto extension = LowerAscii(a_path.extension().string());
			if (extension == ".esl" || (a_header.flags & TES4_FLAG_ESL) != 0) {
				return ModuleKind::Light;
			}
			return ModuleKind::Full;
		}

		struct InflateStream
		{
			z_stream stream{};
			bool initialized{ false };

			InflateStream() = default;
			InflateStream(const InflateStream&) = delete;
			InflateStream& operator=(const InflateStream&) = delete;

			~InflateStream()
			{
				if (initialized) {
					inflateEnd(std::addressof(stream));
				}
			}

			void Reset(std::span<const std::uint8_t> a_compressedData)
			{
				if (a_compressedData.size() < sizeof(std::uint32_t)) {
					throw std::runtime_error("compressed record is missing uncompressed size");
				}

				if (!initialized) {
					if (inflateInit(std::addressof(stream)) != Z_OK) {
						throw std::runtime_error("zlib failed to initialize streaming inflate");
					}
					initialized = true;
				} else if (inflateReset(std::addressof(stream)) != Z_OK) {
					throw std::runtime_error("zlib failed to reset streaming inflate");
				}

				stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(a_compressedData.data() + sizeof(std::uint32_t)));
				stream.avail_in = static_cast<uInt>(a_compressedData.size() - sizeof(std::uint32_t));
			}
		};

		struct ParserContext
		{
			const LoadOrderEntry& entry;
			std::span<const LoadOrderEntry> loadOrder;
			PluginParser::ParseMode mode{ PluginParser::ParseMode::FullRaw };
			std::size_t sourceFileIndex{ 0 };
			std::shared_ptr<const std::vector<FileID>> sourceMasterFileIDs;
			std::vector<GroupPathEntry> groupPath;
			std::optional<FormID> currentWorld;
			std::optional<FormID> currentCell;
			std::vector<std::uint8_t> inflateScratch;
			std::vector<std::uint8_t> cellScanScratch;
			InflateStream cellInflateStream;
		};

		void UpdateContextForGroup(ParserContext& a_context, const GroupHeader& a_group)
		{
			if (a_group.type != 1 && a_group.type != 6 && a_group.type != 8 && a_group.type != 9 && a_group.type != 10) {
				return;
			}

			const auto parentID = ConvertContextLocalFormID(FormID{ a_group.label }, a_context.entry.fileID, a_context.sourceMasterFileIDs);
			if (a_group.type == 1) {
				a_context.currentWorld = parentID;
			} else {
				a_context.currentCell = parentID;
			}
		}

		void ParseRecord(std::span<const std::uint8_t> a_bytes, std::size_t a_offset, PluginFile& a_plugin, ParserContext& a_context)
		{
			const auto header = ReadRecordHeader(a_bytes, a_offset);
			const auto retainRecord = ShouldRetainRecord(header.signature, a_context.mode);
			const auto needsContextID = header.signature == SIG_CELL || header.signature == SIG_WRLD;
			if (!retainRecord && !needsContextID) {
				return;
			}

			const auto loadOrderFormID = ConvertContextLocalFormID(header.formID, a_context.entry.fileID, a_context.sourceMasterFileIDs);
			if (!retainRecord) {
				if (header.signature == SIG_WRLD) {
					a_context.currentWorld = loadOrderFormID;
				} else if (header.signature == SIG_CELL) {
					a_context.currentCell = loadOrderFormID;
				}
				return;
			}

			const auto metadataOnly =
				a_context.mode == PluginParser::ParseMode::StaticWorldIndex &&
				((header.flags & ((1u << 5) | (1u << 12))) != 0);

			std::span<const std::uint8_t> recordData = a_bytes.subspan(a_offset + 24, header.dataSize);
			std::vector<std::uint8_t> decompressed;
			if (!metadataOnly && (header.flags & RECORD_FLAG_COMPRESSED) != 0) {
				decompressed = InflateRecord(recordData);
				recordData = decompressed;
			}

			RawRecord record{};
			record.signature = header.signature;
			record.dataSize = metadataOnly ? 0u : static_cast<std::uint32_t>(recordData.size());
			record.flags = header.flags;
			record.fileFormID = header.formID;
			record.loadOrderFormID = loadOrderFormID;
			record.revision = header.revision;
			record.formVersion = header.formVersion;
			record.unknown = header.unknown;
			record.sourceFileIndex = a_context.sourceFileIndex;
			record.sourceFileID = a_context.entry.fileID;
			record.sourceMasterFileIDs = a_context.sourceMasterFileIDs;
			record.rawOffset = static_cast<std::uint64_t>(a_offset);
			if (a_context.mode == PluginParser::ParseMode::FullRaw) {
				record.groupPath = a_context.groupPath;
			}
			record.parentWorld = a_context.currentWorld;
			record.parentCell = a_context.currentCell;
			if (metadataOnly) {
				record.dataPtr = nullptr;
			} else if (decompressed.empty()) {
				record.dataPtr = recordData.data();
			} else {
				record.ownedData = std::move(decompressed);
				record.dataPtr = record.ownedData.data();
			}
			if (!metadataOnly) {
				record.subrecords = ParseSubrecords(record.Data(), record.signature, a_context.mode);
			}

			if (record.signature == SIG_CELL) {
				record.parentWorld = a_context.currentWorld;
				a_context.currentCell = record.loadOrderFormID;
			}

			if (record.signature == SIG_WRLD) {
				a_context.currentWorld = record.loadOrderFormID;
			}

			const auto index = a_plugin.records.size();
			a_plugin.records.push_back(std::move(record));
			if (a_context.mode == PluginParser::ParseMode::FullRaw) {
				a_plugin.recordsBySignature[header.signature].push_back(index);
			}
		}

		void ParseRange(std::span<const std::uint8_t> a_bytes, std::size_t a_begin, std::size_t a_end, PluginFile& a_plugin, ParserContext& a_context)
		{
			std::size_t offset = a_begin;
			while (offset + 24 <= a_end) {
				const auto signature = ReadLE<FourCC>(a_bytes, offset);
				if (signature == SIG_GRUP) {
					const auto group = ReadGroupHeader(a_bytes, offset);
					if (group.groupSize < 24 || offset + group.groupSize > a_end) {
						throw std::runtime_error("invalid GRUP size in " + a_plugin.entry.pluginName);
					}

					GroupPathEntry pathEntry{};
					pathEntry.type = group.type;
					pathEntry.label = group.label;
					pathEntry.gridX = group.gridX;
					pathEntry.gridY = group.gridY;
					a_context.groupPath.push_back(pathEntry);

					const auto savedWorld = a_context.currentWorld;
					const auto savedCell = a_context.currentCell;
					UpdateContextForGroup(a_context, group);
					ParseRange(a_bytes, offset + 24, offset + group.groupSize, a_plugin, a_context);
					a_context.currentWorld = savedWorld;
					a_context.currentCell = savedCell;
					a_context.groupPath.pop_back();
					offset += group.groupSize;
				} else {
					const auto header = ReadRecordHeader(a_bytes, offset);
					if (offset + 24 + header.dataSize > a_end) {
						throw std::runtime_error("invalid record size in " + a_plugin.entry.pluginName);
					}
					ParseRecord(a_bytes, offset, a_plugin, a_context);
					offset += 24 + header.dataSize;
				}
			}
		}

		[[nodiscard]] BaseObjectKind BaseKindForSignature(FourCC a_signature)
		{
			if (a_signature == SIG_STAT) {
				return BaseObjectKind::Static;
			}
			if (a_signature == SIG_MSTT) {
				return BaseObjectKind::MovableStatic;
			}
			if (a_signature == SIG_TREE) {
				return BaseObjectKind::Tree;
			}
			if (a_signature == SIG_ACTI) {
				return BaseObjectKind::Activator;
			}
			if (a_signature == SIG_DOOR) {
				return BaseObjectKind::Door;
			}
			if (a_signature == SIG_CONT) {
				return BaseObjectKind::Container;
			}
			if (a_signature == SIG_FURN) {
				return BaseObjectKind::Furniture;
			}
			if (a_signature == SIG_LIGH) {
				return BaseObjectKind::Light;
			}
			if (a_signature == SIG_FLOR) {
				return BaseObjectKind::Flora;
			}
			if (a_signature == SIG_SCOL) {
				return BaseObjectKind::StaticCollection;
			}
			if (a_signature == SIG_TACT) {
				return BaseObjectKind::TalkingActivator;
			}
			return BaseObjectKind::Unknown;
		}

		[[nodiscard]] bool IsBaseObjectSignature(FourCC a_signature)
		{
			return BaseKindForSignature(a_signature) != BaseObjectKind::Unknown;
		}

		void ParseAlternateTextures(
			std::vector<AlternateTextureOverride>& a_output,
			FourCC a_subrecordSignature,
			std::span<const std::uint8_t> a_payload,
			const RawRecord* a_record = nullptr,
			FileID a_sourceFileID = {},
			const std::shared_ptr<const std::vector<FileID>>& a_sourceMasterFileIDs = {})
		{
			if (a_payload.size() < 4) {
				return;
			}

			const auto count = ReadLE<std::uint32_t>(a_payload, 0);
			std::size_t offset = 4;
			for (std::uint32_t i = 0; i < count && offset + 4 <= a_payload.size(); ++i) {
				const auto nameSize = ReadLE<std::uint32_t>(a_payload, offset);
				offset += 4;
				if (nameSize > a_payload.size() - offset || a_payload.size() - offset - nameSize < 8) {
					break;
				}

				std::string name{
					reinterpret_cast<const char*>(a_payload.data() + offset),
					reinterpret_cast<const char*>(a_payload.data() + offset + nameSize)
				};
				while (!name.empty() && name.back() == '\0') {
					name.pop_back();
				}
				offset += nameSize;

				const FormID textureSetFileID{ ReadLE<std::uint32_t>(a_payload, offset) };
				offset += 4;
				const auto index3D = ReadLE<std::uint32_t>(a_payload, offset);
				offset += 4;

				FormID textureSetFormID{};
				if (a_record) {
					textureSetFormID = ConvertRecordLocalFormID(textureSetFileID, *a_record);
				} else {
					textureSetFormID = ConvertContextLocalFormID(textureSetFileID, a_sourceFileID, a_sourceMasterFileIDs);
				}

				a_output.push_back(AlternateTextureOverride{
					.subrecordSignature = a_subrecordSignature,
					.name3D = std::move(name),
					.textureSetFormID = textureSetFormID,
					.index3D = index3D
				});
			}
		}

		[[nodiscard]] std::optional<BaseObjectInfo> ExtractBaseObject(const RawRecord& a_record)
		{
			if (!IsBaseObjectSignature(a_record.signature) || a_record.IsDeleted() || a_record.IsIgnored()) {
				return std::nullopt;
			}

			BaseObjectInfo info{};
			info.formID = a_record.loadOrderFormID;
			info.signature = a_record.signature;
			info.kind = BaseKindForSignature(a_record.signature);
			info.flags = a_record.flags;

			if (const auto* edid = FindSubrecord(a_record, SIG_EDID)) {
				info.editorID = ReadString(SubrecordData(a_record, *edid));
			}
			if (const auto* model = FindSubrecord(a_record, SIG_MODL)) {
				info.modelPath = ReadString(SubrecordData(a_record, *model));
			}
			if (a_record.signature == SIG_STAT) {
				if (const auto* lod = FindSubrecord(a_record, SIG_MNAM)) {
					const auto lodData = SubrecordData(a_record, *lod);
					for (std::size_t i = 0; i < info.lodModels.size(); ++i) {
						info.lodModels[i] = ReadString260(lodData, i);
					}
				}
			}
			for (const auto& subrecord : a_record.subrecords) {
				if (IsModelTextureSwapSubrecord(subrecord.signature)) {
					ParseAlternateTextures(
						info.alternateTextures,
						subrecord.signature,
						SubrecordData(a_record, subrecord),
						std::addressof(a_record));
				}
			}
			return info;
		}

		[[nodiscard]] std::optional<TextureSetInfo> ExtractTextureSet(const RawRecord& a_record)
		{
			if (a_record.signature != SIG_TXST || a_record.IsDeleted() || a_record.IsIgnored()) {
				return std::nullopt;
			}

			TextureSetInfo info{};
			info.formID = a_record.loadOrderFormID;
			info.flags = a_record.flags;
			if (const auto* edid = FindSubrecord(a_record, SIG_EDID)) {
				info.editorID = ReadString(SubrecordData(a_record, *edid));
			}
			for (const auto& subrecord : a_record.subrecords) {
				if (const auto slot = TextureSlotIndexForSubrecord(subrecord.signature)) {
					info.texturePaths[*slot] = ReadString(SubrecordData(a_record, subrecord));
				} else if (subrecord.signature == SIG_DNAM && subrecord.size >= 2) {
					info.dnamFlags = ReadLE<std::uint16_t>(SubrecordData(a_record, subrecord), 0);
				}
			}
			return info;
		}

		[[nodiscard]] std::array<float, LandInfo::VertexCount> DecodeLandHeightsInternal(std::span<const std::uint8_t> a_vhgtPayload)
		{
			if (a_vhgtPayload.size() < sizeof(float) + LandInfo::VertexCount) {
				throw std::runtime_error("LAND VHGT payload is too small");
			}

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

		[[nodiscard]] LandTextureInfo ExtractLandTextureFromData(
			const RecordHeader& a_header,
			FormID a_formID,
			FileID a_sourceFileID,
			const std::shared_ptr<const std::vector<FileID>>& a_sourceMasterFileIDs,
			std::span<const std::uint8_t> a_data)
		{
			LandTextureInfo info{};
			info.formID = a_formID;
			info.flags = a_header.flags;

			SubrecordCursor cursor(a_data);
			RawSubrecord subrecord{};
			std::span<const std::uint8_t> payload;
			while (cursor.Next(subrecord, payload)) {
				if (subrecord.signature == SIG_EDID) {
					info.editorID = ReadString(payload);
				} else if (subrecord.signature == SIG_HNAM && payload.size() >= 2) {
					info.havokFriction = payload[payload.size() >= 3 ? 1 : 0];
					info.havokRestitution = payload[payload.size() >= 3 ? 2 : 1];
				} else if (subrecord.signature == SIG_SNAM && payload.size() >= 1) {
					info.textureSpecular = payload[0];
				} else if (subrecord.signature == SIG_GNAM && payload.size() >= 4) {
					info.grassFormIDs.push_back(ConvertContextLocalFormID(
						FormID{ ReadLE<std::uint32_t>(payload, 0) },
						a_sourceFileID,
						a_sourceMasterFileIDs));
				} else if (subrecord.signature == SIG_TNAM && payload.size() >= 4) {
					info.textureSetFormID = ConvertContextLocalFormID(
						FormID{ ReadLE<std::uint32_t>(payload, 0) },
						a_sourceFileID,
						a_sourceMasterFileIDs);
				} else if (subrecord.signature == SIG_MNAM && payload.size() >= 4) {
					info.materialFormID = ConvertContextLocalFormID(
						FormID{ ReadLE<std::uint32_t>(payload, 0) },
						a_sourceFileID,
						a_sourceMasterFileIDs);
				} else if (subrecord.signature == SIG_INAM && payload.size() >= 4) {
					info.materialFlags = ReadLE<std::uint32_t>(payload, 0);
				}
			}
			return info;
		}

		[[nodiscard]] std::optional<LandTextureInfo> ExtractLandTexture(const RawRecord& a_record)
		{
			if (a_record.signature != SIG_LTEX || a_record.IsDeleted() || a_record.IsIgnored()) {
				return std::nullopt;
			}

			return ExtractLandTextureFromData(
				RecordHeader{
					.signature = a_record.signature,
					.dataSize = a_record.dataSize,
					.flags = a_record.flags,
					.formID = a_record.fileFormID,
					.revision = a_record.revision,
					.formVersion = a_record.formVersion,
					.unknown = a_record.unknown,
				},
				a_record.loadOrderFormID,
				a_record.sourceFileID,
				a_record.sourceMasterFileIDs,
				a_record.Data());
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

		[[nodiscard]] GrassInfo ExtractGrassFromData(
			const RecordHeader& a_header,
			FormID a_formID,
			std::span<const std::uint8_t> a_data)
		{
			GrassInfo info{};
			info.formID = a_formID;
			info.flags = a_header.flags;

			SubrecordCursor cursor(a_data);
			RawSubrecord subrecord{};
			std::span<const std::uint8_t> payload;
			while (cursor.Next(subrecord, payload)) {
				if (subrecord.signature == SIG_EDID) {
					info.editorID = ReadString(payload);
				} else if (subrecord.signature == SIG_MODL) {
					info.modelPath = ReadString(payload);
				} else if (subrecord.signature == SIG_OBND) {
					info.bounds = ReadObjectBounds(payload);
				} else if (subrecord.signature == SIG_DATA && payload.size() >= 0x20) {
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

		[[nodiscard]] std::optional<GrassInfo> ExtractGrass(const RawRecord& a_record)
		{
			if (a_record.signature != SIG_GRAS || a_record.IsDeleted() || a_record.IsIgnored()) {
				return std::nullopt;
			}
			return ExtractGrassFromData(
				RecordHeader{
					.signature = a_record.signature,
					.dataSize = a_record.dataSize,
					.flags = a_record.flags,
					.formID = a_record.fileFormID,
					.revision = a_record.revision,
					.formVersion = a_record.formVersion,
					.unknown = a_record.unknown,
				},
				a_record.loadOrderFormID,
				a_record.Data());
		}

		[[nodiscard]] std::optional<CellInfo> ExtractCell(const RawRecord& a_record)
		{
			if (a_record.signature != SIG_CELL || a_record.IsDeleted() || a_record.IsIgnored()) {
				return std::nullopt;
			}

			CellInfo info{};
			info.formID = a_record.loadOrderFormID;
			info.worldFormID = a_record.parentWorld;
			info.flags = a_record.flags;
			if (const auto* edid = FindSubrecord(a_record, SIG_EDID)) {
				info.editorID = ReadString(SubrecordData(a_record, *edid));
			}
			if (const auto* data = FindSubrecord(a_record, SIG_DATA)) {
				const auto bytes = SubrecordData(a_record, *data);
				if (bytes.size() == 1) {
					info.cellFlags = bytes[0];
				} else if (bytes.size() >= 2) {
					info.cellFlags = ReadLE<std::uint16_t>(bytes, 0);
				}
			}
			if (const auto* xclc = FindSubrecord(a_record, SIG_XCLC); xclc && xclc->size >= 8) {
				const auto bytes = SubrecordData(a_record, *xclc);
				info.gridX = ReadLE<std::int32_t>(bytes, 0);
				info.gridY = ReadLE<std::int32_t>(bytes, 4);
			}
			if (const auto* xclw = FindSubrecord(a_record, SIG_XCLW); xclw && xclw->size >= 4) {
				const auto waterHeight = ReadLE<float>(SubrecordData(a_record, *xclw), 0);
				if (IsValidCellWaterHeight(waterHeight)) {
					info.waterHeight = waterHeight;
				}
			}
			return info;
		}

		[[nodiscard]] LandInfo ExtractLandFromData(
			const RecordHeader& a_header,
			FormID a_formID,
			std::optional<FormID> a_parentWorld,
			std::optional<FormID> a_parentCell,
			FileID a_sourceFileID,
			const std::shared_ptr<const std::vector<FileID>>& a_sourceMasterFileIDs,
			std::span<const std::uint8_t> a_data)
		{
			LandInfo info{};
			info.formID = a_formID;
			info.flags = a_header.flags;
			info.parentCell = a_parentCell.value_or(FormID{});
			info.worldFormID = a_parentWorld;
			for (auto& color : info.vertexColors) {
				color = { 255, 255, 255 };
			}

			std::optional<std::uint16_t> currentLayerIndex;
			std::optional<std::uint8_t> currentQuadrant;
			SubrecordCursor cursor(a_data);
			RawSubrecord subrecord{};
			std::span<const std::uint8_t> payload;
			while (cursor.Next(subrecord, payload)) {
				if (subrecord.signature == SIG_VHGT && payload.size() >= sizeof(float) + LandInfo::VertexCount) {
					info.heights = DecodeLandHeightsInternal(payload);
					info.hasHeights = true;
				} else if (subrecord.signature == SIG_VNML && payload.size() >= LandInfo::VertexCount * 3) {
					for (std::size_t i = 0; i < LandInfo::VertexCount; ++i) {
						info.sourceNormals[i] = {
							static_cast<std::int8_t>(payload[i * 3 + 0]),
							static_cast<std::int8_t>(payload[i * 3 + 1]),
							static_cast<std::int8_t>(payload[i * 3 + 2])
						};
					}
					info.hasSourceNormals = true;
				} else if (subrecord.signature == SIG_VCLR && payload.size() >= LandInfo::VertexCount * 3) {
					for (std::size_t i = 0; i < LandInfo::VertexCount; ++i) {
						info.vertexColors[i] = {
							payload[i * 3 + 0],
							payload[i * 3 + 1],
							payload[i * 3 + 2]
						};
					}
					info.hasVertexColors = true;
				} else if (subrecord.signature == SIG_BTXT && payload.size() >= 8) {
					const auto quadrant = payload[4];
					if (quadrant < LandInfo::QuadrantCount) {
						info.baseTextures[quadrant] = LandBaseTexture{
							.landTextureFormID = ResolveDefaultLandTexture(ConvertContextLocalFormID(
								FormID{ ReadLE<std::uint32_t>(payload, 0) },
								a_sourceFileID,
								a_sourceMasterFileIDs)),
							.quadrant = quadrant,
						};
					}
					currentLayerIndex.reset();
					currentQuadrant.reset();
				} else if (subrecord.signature == SIG_ATXT && payload.size() >= 8) {
					const auto quadrant = payload[4];
					const auto layerIndex = ReadLE<std::uint16_t>(payload, 6);
					info.alphaTextures.push_back(LandAlphaTexture{
						.landTextureFormID = ResolveDefaultLandTexture(ConvertContextLocalFormID(
							FormID{ ReadLE<std::uint32_t>(payload, 0) },
							a_sourceFileID,
							a_sourceMasterFileIDs)),
						.quadrant = quadrant,
						.layerIndex = layerIndex,
					});
					currentQuadrant = quadrant;
					currentLayerIndex = layerIndex;
				} else if (subrecord.signature == SIG_VTXT && currentLayerIndex && currentQuadrant) {
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

		[[nodiscard]] std::optional<LandInfo> ExtractLand(const RawRecord& a_record)
		{
			if (a_record.signature != SIG_LAND || a_record.IsDeleted() || a_record.IsIgnored()) {
				return std::nullopt;
			}

			return ExtractLandFromData(
				RecordHeader{
					.signature = a_record.signature,
					.dataSize = a_record.dataSize,
					.flags = a_record.flags,
					.formID = a_record.fileFormID,
					.revision = a_record.revision,
					.formVersion = a_record.formVersion,
					.unknown = a_record.unknown,
				},
				a_record.loadOrderFormID,
				a_record.parentWorld,
				a_record.parentCell,
				a_record.sourceFileID,
				a_record.sourceMasterFileIDs,
				a_record.Data());
		}

		[[nodiscard]] std::optional<PlacementInfo> ExtractPlacement(const RawRecord& a_record)
		{
			if ((a_record.signature != SIG_REFR && a_record.signature != SIG_ACHR) || a_record.IsDeleted() || a_record.IsIgnored()) {
				return std::nullopt;
			}

			const auto* base = FindSubrecord(a_record, SIG_NAME);
			if (!base || base->size < 4) {
				return std::nullopt;
			}
			const auto baseData = SubrecordData(a_record, *base);

			PlacementInfo info{};
			info.formID = a_record.loadOrderFormID;
			info.parentCell = a_record.parentCell.value_or(FormID{});
			info.worldFormID = a_record.parentWorld;
			info.signature = a_record.signature;
			info.flags = a_record.flags;
			info.baseFormID = ConvertRecordLocalFormID(FormID{ ReadLE<std::uint32_t>(baseData, 0) }, a_record);

			if (const auto* data = FindSubrecord(a_record, SIG_DATA); data && data->size >= 24) {
				const auto bytes = SubrecordData(a_record, *data);
				for (std::size_t i = 0; i < 3; ++i) {
					info.position[i] = ReadLE<float>(bytes, i * 4);
					info.rotation[i] = ReadLE<float>(bytes, 12 + i * 4);
				}
			}
			if (const auto* scale = FindSubrecord(a_record, SIG_XSCL); scale && scale->size >= 4) {
				info.scale = ReadLE<float>(SubrecordData(a_record, *scale), 0);
			}
			if (const auto* xesp = FindSubrecord(a_record, SIG_XESP); xesp && xesp->size >= 8) {
				const auto bytes = SubrecordData(a_record, *xesp);
				info.enableParent = ConvertRecordLocalFormID(FormID{ ReadLE<std::uint32_t>(bytes, 0) }, a_record);
				info.enableParentFlags = ReadLE<std::uint32_t>(bytes, 4);
			}
			return info;
		}

		[[nodiscard]] bool IsSuppressedFlags(std::uint32_t a_flags)
		{
			return (a_flags & ((1u << 5) | (1u << 12))) != 0;
		}

		[[nodiscard]] WorldInfo ExtractWorldFromData(
			const RecordHeader& a_header,
			FormID a_formID,
			std::span<const std::uint8_t> a_data)
		{
			WorldInfo info{};
			info.formID = a_formID;
			info.flags = a_header.flags;
			SubrecordCursor cursor(a_data);
			RawSubrecord subrecord{};
			std::span<const std::uint8_t> payload;
			while (cursor.Next(subrecord, payload)) {
				if (subrecord.signature == SIG_EDID) {
					info.editorID = ReadString(payload);
					break;
				}
			}
			return info;
		}

		[[nodiscard]] BaseObjectInfo ExtractBaseObjectFromData(
			const RecordHeader& a_header,
			FormID a_formID,
			FileID a_sourceFileID,
			const std::shared_ptr<const std::vector<FileID>>& a_sourceMasterFileIDs,
			std::span<const std::uint8_t> a_data)
		{
			BaseObjectInfo info{};
			info.formID = a_formID;
			info.signature = a_header.signature;
			info.kind = BaseKindForSignature(a_header.signature);
			info.flags = a_header.flags;

			SubrecordCursor cursor(a_data);
			RawSubrecord subrecord{};
			std::span<const std::uint8_t> payload;
			while (cursor.Next(subrecord, payload)) {
				if (subrecord.signature == SIG_MODL) {
					info.modelPath = ReadString(payload);
				} else if (subrecord.signature == SIG_OBND) {
					info.bounds = ReadObjectBounds(payload);
				} else if (a_header.signature == SIG_STAT && subrecord.signature == SIG_MNAM) {
					for (std::size_t i = 0; i < info.lodModels.size(); ++i) {
						info.lodModels[i] = ReadString260(payload, i);
					}
				} else if (IsModelTextureSwapSubrecord(subrecord.signature)) {
					ParseAlternateTextures(
						info.alternateTextures,
						subrecord.signature,
						payload,
						nullptr,
						a_sourceFileID,
						a_sourceMasterFileIDs);
				}
			}
			return info;
		}

		[[nodiscard]] TextureSetInfo ExtractTextureSetFromData(
			const RecordHeader& a_header,
			FormID a_formID,
			std::span<const std::uint8_t> a_data)
		{
			TextureSetInfo info{};
			info.formID = a_formID;
			info.flags = a_header.flags;

			SubrecordCursor cursor(a_data);
			RawSubrecord subrecord{};
			std::span<const std::uint8_t> payload;
			while (cursor.Next(subrecord, payload)) {
				if (subrecord.signature == SIG_EDID) {
					info.editorID = ReadString(payload);
				} else if (const auto slot = TextureSlotIndexForSubrecord(subrecord.signature)) {
					info.texturePaths[*slot] = ReadString(payload);
				} else if (subrecord.signature == SIG_DNAM && payload.size() >= 2) {
					info.dnamFlags = ReadLE<std::uint16_t>(payload, 0);
				}
			}
			return info;
		}

		[[nodiscard]] CellInfo ExtractCellFromData(
			const RecordHeader& a_header,
			FormID a_formID,
			std::optional<FormID> a_parentWorld,
			std::span<const std::uint8_t> a_data)
		{
			CellInfo info{};
			info.formID = a_formID;
			info.worldFormID = a_parentWorld;
			info.flags = a_header.flags;

			SubrecordCursor cursor(a_data);
			RawSubrecord subrecord{};
			std::span<const std::uint8_t> payload;
			while (cursor.Next(subrecord, payload)) {
				if (subrecord.signature == SIG_DATA) {
					if (payload.size() == 1) {
						info.cellFlags = payload[0];
					} else if (payload.size() >= 2) {
						info.cellFlags = ReadLE<std::uint16_t>(payload, 0);
					}
				} else if (subrecord.signature == SIG_XCLC && payload.size() >= 8) {
					info.gridX = ReadLE<std::int32_t>(payload, 0);
					info.gridY = ReadLE<std::int32_t>(payload, 4);
				} else if (subrecord.signature == SIG_XCLW && payload.size() >= 4) {
					const auto waterHeight = ReadLE<float>(payload, 0);
					if (IsValidCellWaterHeight(waterHeight)) {
						info.waterHeight = waterHeight;
					}
				}
			}
			return info;
		}

		struct StreamingCellScan
		{
			CellInfo info;
			std::vector<std::uint8_t>* pending{ nullptr };
			std::size_t consumed{ 0 };
			bool hasData{ false };
			bool hasXclc{ false };
			bool hasXclw{ false };
			std::size_t outputBytes{ 0 };

			[[nodiscard]] bool Complete() const noexcept
			{
				return hasData && hasXclc && hasXclw;
			}
		};

		void ProcessCellSubrecord(StreamingCellScan& a_scan, FourCC a_signature, std::span<const std::uint8_t> a_payload)
		{
			if (a_signature == SIG_DATA) {
				if (a_payload.size() == 1) {
					a_scan.info.cellFlags = a_payload[0];
					a_scan.hasData = true;
				} else if (a_payload.size() >= 2) {
					a_scan.info.cellFlags = ReadLE<std::uint16_t>(a_payload, 0);
					a_scan.hasData = true;
				}
			} else if (a_signature == SIG_XCLC && a_payload.size() >= 8) {
				a_scan.info.gridX = ReadLE<std::int32_t>(a_payload, 0);
				a_scan.info.gridY = ReadLE<std::int32_t>(a_payload, 4);
				a_scan.hasXclc = true;
			} else if (a_signature == SIG_XCLW && a_payload.size() >= 4) {
				const auto waterHeight = ReadLE<float>(a_payload, 0);
				if (IsValidCellWaterHeight(waterHeight)) {
					a_scan.info.waterHeight = waterHeight;
				}
				a_scan.hasXclw = true;
			}
		}

		[[nodiscard]] bool AppendAndScanCellBytes(StreamingCellScan& a_scan, std::span<const std::uint8_t> a_bytes)
		{
			a_scan.outputBytes += a_bytes.size();
			auto& pending = *a_scan.pending;
			pending.insert(pending.end(), a_bytes.begin(), a_bytes.end());

			for (;;) {
				const auto available = pending.size() - a_scan.consumed;
				if (available < 6) {
					break;
				}

				const auto offset = a_scan.consumed;
				const auto pendingBytes = std::span<const std::uint8_t>(pending);
				auto signature = ReadLE<FourCC>(pendingBytes, offset);
				std::uint32_t size = ReadLE<std::uint16_t>(pendingBytes, offset + 4);
				std::size_t headerSize = 6;

				if (signature == SIG_XXXX) {
					if (available < 16) {
						break;
					}
					size = ReadLE<std::uint32_t>(pendingBytes, offset + 6);
					signature = ReadLE<FourCC>(pendingBytes, offset + 10);
					headerSize = 16;
				}

				const auto totalSize = headerSize + static_cast<std::size_t>(size);
				if (available < totalSize) {
					break;
				}

				ProcessCellSubrecord(
					a_scan,
					signature,
					std::span<const std::uint8_t>(pending).subspan(offset + headerSize, size));
				a_scan.consumed += totalSize;
				if (a_scan.Complete()) {
					return true;
				}
			}

			if (a_scan.consumed > 4096 && a_scan.consumed * 2 >= pending.size()) {
				pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(a_scan.consumed));
				a_scan.consumed = 0;
			}
			return false;
		}

		[[nodiscard]] CellInfo ExtractCellFromCompressedData(
			const RecordHeader& a_header,
			FormID a_formID,
			std::optional<FormID> a_parentWorld,
			std::span<const std::uint8_t> a_compressedData,
			std::vector<std::uint8_t>& a_pendingScratch,
			InflateStream& a_inflateStream,
			StaticPluginStats& a_stats)
		{
			if (a_compressedData.size() < sizeof(std::uint32_t)) {
				throw std::runtime_error("compressed CELL record is missing uncompressed size");
			}

			StreamingCellScan scan{};
			scan.info.formID = a_formID;
			scan.info.worldFormID = a_parentWorld;
			scan.info.flags = a_header.flags;
			a_pendingScratch.clear();
			if (a_pendingScratch.capacity() < 256) {
				a_pendingScratch.reserve(256);
			}
			scan.pending = std::addressof(a_pendingScratch);

			a_inflateStream.Reset(a_compressedData);
			auto& stream = a_inflateStream.stream;

			std::array<std::uint8_t, 64> output{};
			bool earlyStop = false;
			for (;;) {
				stream.next_out = reinterpret_cast<Bytef*>(output.data());
				stream.avail_out = static_cast<uInt>(output.size());
				const auto result = inflate(std::addressof(stream), Z_NO_FLUSH);
				if (result != Z_OK && result != Z_STREAM_END) {
					throw std::runtime_error("zlib failed to stream inflate CELL record");
				}

				const auto produced = output.size() - stream.avail_out;
				if (produced != 0 && AppendAndScanCellBytes(scan, std::span<const std::uint8_t>(output.data(), produced))) {
					earlyStop = true;
					break;
				}
				if (result == Z_STREAM_END) {
					break;
				}
				if (produced == 0 && stream.avail_in == 0) {
					break;
				}
			}

			++a_stats.streamedCellRecords;
			a_stats.streamedCellOutputBytes += scan.outputBytes;
			if (earlyStop) {
				++a_stats.streamedCellEarlyStops;
			}
			return scan.info;
		}

		[[nodiscard]] std::optional<PlacementInfo> ExtractPlacementFromData(
			const RecordHeader& a_header,
			FormID a_formID,
			std::optional<FormID> a_parentWorld,
			std::optional<FormID> a_parentCell,
			FileID a_sourceFileID,
			const std::shared_ptr<const std::vector<FileID>>& a_sourceMasterFileIDs,
			std::span<const std::uint8_t> a_data)
		{
			PlacementInfo info{};
			info.formID = a_formID;
			info.parentCell = a_parentCell.value_or(FormID{});
			info.worldFormID = a_parentWorld;
			info.signature = a_header.signature;
			info.flags = a_header.flags;

			bool hasBase = false;
			SubrecordCursor cursor(a_data);
			RawSubrecord subrecord{};
			std::span<const std::uint8_t> payload;
			while (cursor.Next(subrecord, payload)) {
				if (subrecord.signature == SIG_NAME && payload.size() >= 4) {
					info.baseFormID = ConvertContextLocalFormID(FormID{ ReadLE<std::uint32_t>(payload, 0) }, a_sourceFileID, a_sourceMasterFileIDs);
					hasBase = true;
				} else if (subrecord.signature == SIG_DATA && payload.size() >= 24) {
					for (std::size_t i = 0; i < 3; ++i) {
						info.position[i] = ReadLE<float>(payload, i * 4);
						info.rotation[i] = ReadLE<float>(payload, 12 + i * 4);
					}
				} else if (subrecord.signature == SIG_XSCL && payload.size() >= 4) {
					info.scale = ReadLE<float>(payload, 0);
				} else if (subrecord.signature == SIG_XESP && payload.size() >= 8) {
					info.enableParent = ConvertContextLocalFormID(FormID{ ReadLE<std::uint32_t>(payload, 0) }, a_sourceFileID, a_sourceMasterFileIDs);
					info.enableParentFlags = ReadLE<std::uint32_t>(payload, 4);
				}
			}

			if (!hasBase) {
				return std::nullopt;
			}
			return info;
		}

		void AddSuppressor(
			StaticPluginShard& a_shard,
			const RecordHeader& a_header,
			FormID a_formID,
			std::size_t a_sourceFileIndex)
		{
			if (a_formID.IsNull() || a_formID.IsNone()) {
				return;
			}
			a_shard.suppressors.push_back(StaticRecordSuppressor{
				.formID = a_formID,
				.signature = a_header.signature,
				.flags = a_header.flags,
				.sourceFileIndex = a_sourceFileIndex,
			});
			++a_shard.stats.suppressors;
		}

		void ParseStaticRecord(std::span<const std::uint8_t> a_bytes, std::size_t a_offset, StaticPluginShard& a_shard, ParserContext& a_context)
		{
			const auto header = ReadRecordHeader(a_bytes, a_offset);
			++a_shard.stats.recordsScanned;

			const auto needsTypedPayload =
				header.signature == SIG_WRLD ||
				header.signature == SIG_CELL ||
				header.signature == SIG_LAND ||
				header.signature == SIG_REFR ||
				header.signature == SIG_ACHR ||
				header.signature == SIG_TXST ||
				header.signature == SIG_LTEX ||
				header.signature == SIG_GRAS ||
				IsBaseObjectSignature(header.signature);
			if (!needsTypedPayload) {
				return;
			}

			const auto loadOrderFormID = ConvertContextLocalFormID(header.formID, a_context.entry.fileID, a_context.sourceMasterFileIDs);
			const auto suppressed = IsSuppressedFlags(header.flags);
			if (suppressed) {
				AddSuppressor(a_shard, header, loadOrderFormID, a_context.sourceFileIndex);
				if (header.signature == SIG_WRLD) {
					a_context.currentWorld = loadOrderFormID;
				} else if (header.signature == SIG_CELL) {
					a_context.currentCell = loadOrderFormID;
				}
				return;
			}

			std::span<const std::uint8_t> recordData = a_bytes.subspan(a_offset + 24, header.dataSize);
			if ((header.flags & RECORD_FLAG_COMPRESSED) != 0) {
				const auto uncompressedSize = recordData.size() >= sizeof(std::uint32_t) ? ReadLE<std::uint32_t>(recordData, 0) : 0u;
				const auto inflateBegin = Clock::now();
				++a_shard.stats.compressedRecords;
				if (IsBaseObjectSignature(header.signature)) {
					++a_shard.stats.compressedBaseRecords;
				} else if (header.signature == SIG_WRLD) {
					++a_shard.stats.compressedWorldRecords;
				} else if (header.signature == SIG_CELL) {
					++a_shard.stats.compressedCellRecords;
				} else if (header.signature == SIG_LAND) {
					++a_shard.stats.compressedLandRecords;
				} else if (header.signature == SIG_REFR || header.signature == SIG_ACHR) {
					++a_shard.stats.compressedPlacementRecords;
				}

				if (header.signature == SIG_CELL) {
					a_shard.cells.push_back(ExtractCellFromCompressedData(
						header,
						loadOrderFormID,
						a_context.currentWorld,
						recordData,
						a_context.cellScanScratch,
						a_context.cellInflateStream,
						a_shard.stats));
					a_shard.stats.inflateMilliseconds += ElapsedMilliseconds(inflateBegin);
					a_shard.stats.uncompressedRecordBytes += uncompressedSize;
					++a_shard.stats.typedRecords;
					++a_shard.stats.cells;
					a_context.currentCell = loadOrderFormID;
					return;
				}

				InflateRecordInto(recordData, a_context.inflateScratch);
				a_shard.stats.inflateMilliseconds += ElapsedMilliseconds(inflateBegin);
				recordData = a_context.inflateScratch;
			}
			a_shard.stats.uncompressedRecordBytes += recordData.size();
			++a_shard.stats.typedRecords;

			if (header.signature == SIG_WRLD) {
				a_shard.worlds.push_back(ExtractWorldFromData(header, loadOrderFormID, recordData));
				++a_shard.stats.worlds;
				a_context.currentWorld = loadOrderFormID;
			} else if (header.signature == SIG_CELL) {
				a_shard.cells.push_back(ExtractCellFromData(header, loadOrderFormID, a_context.currentWorld, recordData));
				++a_shard.stats.cells;
				a_context.currentCell = loadOrderFormID;
			} else if (header.signature == SIG_LAND) {
				a_shard.lands.push_back(ExtractLandFromData(
					header,
					loadOrderFormID,
					a_context.currentWorld,
					a_context.currentCell,
					a_context.entry.fileID,
					a_context.sourceMasterFileIDs,
					recordData));
				++a_shard.stats.lands;
			} else if (header.signature == SIG_REFR || header.signature == SIG_ACHR) {
				if (auto placement = ExtractPlacementFromData(
						header,
						loadOrderFormID,
						a_context.currentWorld,
						a_context.currentCell,
						a_context.entry.fileID,
						a_context.sourceMasterFileIDs,
						recordData)) {
					a_shard.placements.push_back(std::move(*placement));
					++a_shard.stats.placements;
				}
			} else if (IsBaseObjectSignature(header.signature)) {
				a_shard.baseObjects.push_back(ExtractBaseObjectFromData(
					header,
					loadOrderFormID,
					a_context.entry.fileID,
					a_context.sourceMasterFileIDs,
					recordData));
				++a_shard.stats.baseObjects;
			} else if (header.signature == SIG_TXST) {
				a_shard.textureSets.push_back(ExtractTextureSetFromData(header, loadOrderFormID, recordData));
			} else if (header.signature == SIG_LTEX) {
				a_shard.landTextures.push_back(ExtractLandTextureFromData(
					header,
					loadOrderFormID,
					a_context.entry.fileID,
					a_context.sourceMasterFileIDs,
					recordData));
				++a_shard.stats.landTextures;
			} else if (header.signature == SIG_GRAS) {
				a_shard.grasses.push_back(ExtractGrassFromData(header, loadOrderFormID, recordData));
			}
		}

		void ParseStaticRange(std::span<const std::uint8_t> a_bytes, std::size_t a_begin, std::size_t a_end, StaticPluginShard& a_shard, ParserContext& a_context)
		{
			std::size_t offset = a_begin;
			while (offset + 24 <= a_end) {
				const auto signature = ReadLE<FourCC>(a_bytes, offset);
				if (signature == SIG_GRUP) {
					const auto group = ReadGroupHeader(a_bytes, offset);
					if (group.groupSize < 24 || offset + group.groupSize > a_end) {
						throw std::runtime_error("invalid GRUP size in " + a_shard.entry.pluginName);
					}

					const auto savedWorld = a_context.currentWorld;
					const auto savedCell = a_context.currentCell;
					UpdateContextForGroup(a_context, group);
					ParseStaticRange(a_bytes, offset + 24, offset + group.groupSize, a_shard, a_context);
					a_context.currentWorld = savedWorld;
					a_context.currentCell = savedCell;
					offset += group.groupSize;
				} else {
					const auto header = ReadRecordHeader(a_bytes, offset);
					if (offset + 24 + header.dataSize > a_end) {
						throw std::runtime_error("invalid record size in " + a_shard.entry.pluginName);
					}
					ParseStaticRecord(a_bytes, offset, a_shard, a_context);
					offset += 24 + header.dataSize;
				}
			}
		}
	}

	std::string FourCCString(FourCC a_value)
	{
		std::string result(4, '\0');
		result[0] = static_cast<char>(a_value & 0xFFu);
		result[1] = static_cast<char>((a_value >> 8) & 0xFFu);
		result[2] = static_cast<char>((a_value >> 16) & 0xFFu);
		result[3] = static_cast<char>((a_value >> 24) & 0xFFu);
		return result;
	}

	FourCC FourCCFromString(std::string_view a_value)
	{
		if (a_value.size() != 4) {
			throw std::runtime_error("FourCC string must be exactly four characters");
		}
		return MakeFourCC(a_value[0], a_value[1], a_value[2], a_value[3]);
	}

	std::string LowerAscii(std::string_view a_value)
	{
		std::string result(a_value);
		std::ranges::transform(result, result.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
		return result;
	}

	std::string Trim(std::string_view a_value)
	{
		auto begin = a_value.begin();
		auto end = a_value.end();
		while (begin != end && std::isspace(static_cast<unsigned char>(*begin))) {
			++begin;
		}
		while (begin != end && std::isspace(static_cast<unsigned char>(*(end - 1)))) {
			--end;
		}
		return std::string(begin, end);
	}

	std::uint32_t FileID::BaseFormID() const
	{
		if (kind == ModuleKind::Light) {
			return 0xFE000000u | (static_cast<std::uint32_t>(slot & 0x0FFFu) << 12);
		}
		return static_cast<std::uint32_t>(slot) << 24;
	}

	std::uint32_t FileID::ObjectMask() const
	{
		return kind == ModuleKind::Light ? 0xFFFu : 0xFFFFFFu;
	}

	std::string FileID::ToString() const
	{
		std::ostringstream out;
		out << std::hex << std::uppercase;
		if (kind == ModuleKind::Light) {
			out << "FE " << std::setw(3) << std::setfill('0') << slot;
		} else {
			out << std::setw(2) << std::setfill('0') << slot;
		}
		return out.str();
	}

	FileID FormID::GetFileID() const
	{
		const auto fullSlot = static_cast<std::uint8_t>(value >> 24);
		if (fullSlot == 0xFE) {
			return FileID{ .kind = ModuleKind::Light, .slot = static_cast<std::uint16_t>((value >> 12) & 0xFFFu) };
		}
		return FileID{ .kind = ModuleKind::Full, .slot = fullSlot };
	}

	std::uint32_t FormID::ObjectID() const
	{
		const auto fileID = GetFileID();
		return value & fileID.ObjectMask();
	}

	FormID FormID::WithFileID(FileID a_fileID) const
	{
		return FormID{ a_fileID.BaseFormID() | (ObjectID() & a_fileID.ObjectMask()) };
	}

	std::string FormID::ToString() const
	{
		std::ostringstream out;
		out << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value;
		return out.str();
	}

	ExplicitLoadOrderSource::ExplicitLoadOrderSource(std::vector<std::filesystem::path> a_pluginPaths) :
		_pluginPaths(std::move(a_pluginPaths))
	{}

	std::vector<LoadOrderEntry> ExplicitLoadOrderSource::Load()
	{
		std::vector<LoadOrderEntry> result;
		result.reserve(_pluginPaths.size());
		for (const auto& path : _pluginPaths) {
			if (!IsPluginExtension(path.filename().string())) {
				continue;
			}
			result.push_back(LoadOrderEntry{
				.path = path,
				.pluginName = PluginNameFromPath(path),
			});
		}
		return result;
	}

	PluginsTxtLoadOrderSource::PluginsTxtLoadOrderSource(std::filesystem::path a_dataPath, std::filesystem::path a_pluginsTxtPath) :
		_dataPath(std::move(a_dataPath)),
		_pluginsTxtPath(std::move(a_pluginsTxtPath))
	{}

	std::vector<LoadOrderEntry> PluginsTxtLoadOrderSource::Load()
	{
		std::ifstream input(_pluginsTxtPath);
		if (!input) {
			throw std::runtime_error("failed to open plugins.txt: " + _pluginsTxtPath.string());
		}

		std::vector<LoadOrderEntry> result;
		std::unordered_map<std::string, std::uint8_t> added;
		const auto appendIfPresent = [&](std::string_view a_pluginName) {
			const auto key = LowerAscii(a_pluginName);
			if (!IsPluginExtension(key) || added.contains(key)) {
				return;
			}
			const auto path = _dataPath / key;
			if (!std::filesystem::exists(path)) {
				return;
			}
			result.push_back(LoadOrderEntry{
				.path = path,
				.pluginName = key,
			});
			added.emplace(key, 0);
		};

		appendIfPresent("Skyrim.esm");
		appendIfPresent("Update.esm");
		appendIfPresent("Dawnguard.esm");
		appendIfPresent("HearthFires.esm");
		appendIfPresent("Dragonborn.esm");

		const auto cccPath = _dataPath.parent_path() / "Skyrim.ccc";
		if (std::filesystem::exists(cccPath)) {
			std::ifstream cccInput(cccPath);
			std::string cccLine;
			while (std::getline(cccInput, cccLine)) {
				const auto trimmed = Trim(cccLine);
				if (!trimmed.empty() && !trimmed.starts_with('#')) {
					appendIfPresent(trimmed);
				}
			}
		}

		std::string line;
		while (std::getline(input, line)) {
			line = Trim(line);
			if (line.empty() || line.starts_with('#')) {
				continue;
			}
			// Skyrim SE/AE and MO2 prefix enabled entries with '*'. Unstarred
			// entries remain in plugins.txt to preserve their ordering, but are
			// disabled and must not contribute records to the resolved load order.
			if (!line.starts_with('*')) {
				continue;
			}
			line = Trim(std::string_view(line).substr(1));
			if (!IsPluginExtension(line)) {
				continue;
			}
			appendIfPresent(line);
		}
		return result;
	}

	const RawRecord* OverrideChain::Winning() const
	{
		return overrides.empty() ? firstRecord : overrides.back();
	}

	const RawRecord* OverrideChain::VisibleFrom(std::size_t a_sourceFileIndex) const
	{
		const RawRecord* result = nullptr;
		if (firstRecord && firstRecord->sourceFileIndex <= a_sourceFileIndex) {
			result = firstRecord;
		}
		for (const auto* record : overrides) {
			if (record && record->sourceFileIndex <= a_sourceFileIndex) {
				result = record;
			}
		}
		return result;
	}

	const OverrideChain* ResolvedRecordStore::Find(FormID a_id) const
	{
		const auto it = chains.find(a_id);
		return it == chains.end() ? nullptr : std::addressof(it->second);
	}

	const RawRecord* ResolvedRecordStore::Winning(FormID a_id) const
	{
		const auto* chain = Find(a_id);
		return chain ? chain->Winning() : nullptr;
	}

	const RawRecord* ResolvedRecordStore::VisibleFrom(FormID a_id, std::size_t a_sourceFileIndex) const
	{
		const auto* chain = Find(a_id);
		return chain ? chain->VisibleFrom(a_sourceFileIndex) : nullptr;
	}

	std::size_t CellKeyHash::operator()(const CellKey& a_key) const noexcept
	{
		auto value = static_cast<std::size_t>(a_key.worldFormID.value);
		value ^= static_cast<std::size_t>(static_cast<std::uint32_t>(a_key.x)) + 0x9e3779b9u + (value << 6) + (value >> 2);
		value ^= static_cast<std::size_t>(static_cast<std::uint32_t>(a_key.y)) + 0x9e3779b9u + (value << 6) + (value >> 2);
		return value;
	}

	std::size_t GameDataSnapshot::RawRecordCount() const
	{
		std::size_t result = 0;
		for (const auto& plugin : plugins) {
			result += plugin.records.size();
		}
		return result;
	}

	PluginHeader PluginParser::ReadHeader(const std::filesystem::path& a_path) const
	{
		std::error_code ec;
		const auto writeTime = std::filesystem::last_write_time(a_path, ec);
		const auto fileSize = ec ? 0 : std::filesystem::file_size(a_path, ec);
		const auto cacheKey = a_path.wstring();
		if (!ec) {
			std::scoped_lock lock(g_headerCacheMutex);
			const auto it = g_headerCache.find(cacheKey);
			if (it != g_headerCache.end() && it->second.writeTime == writeTime && it->second.fileSize == fileSize) {
				return it->second.header;
			}
		}

		std::ifstream input(a_path, std::ios::binary);
		if (!input) {
			throw std::runtime_error("failed to open plugin file: " + a_path.string());
		}

		std::array<std::uint8_t, 24> headerBytes{};
		input.read(reinterpret_cast<char*>(headerBytes.data()), static_cast<std::streamsize>(headerBytes.size()));
		if (input.gcount() != static_cast<std::streamsize>(headerBytes.size())) {
			throw std::runtime_error("plugin is too small: " + a_path.string());
		}

		const auto header = ReadRecordHeader(headerBytes, 0);
		if (header.signature != SIG_TES4) {
			throw std::runtime_error("plugin does not start with TES4 header: " + a_path.string());
		}
		std::vector<std::uint8_t> headerData(header.dataSize);
		if (!headerData.empty()) {
			input.read(reinterpret_cast<char*>(headerData.data()), static_cast<std::streamsize>(headerData.size()));
			if (input.gcount() != static_cast<std::streamsize>(headerData.size())) {
				throw std::runtime_error("TES4 header data exceeds file size: " + a_path.string());
			}
		}

		PluginHeader result{};
		result.flags = header.flags;
		ParseHeaderRecordData(headerData, result);
		if (!ec) {
			std::scoped_lock lock(g_headerCacheMutex);
			g_headerCache.insert_or_assign(cacheKey, HeaderCacheEntry{
				.writeTime = writeTime,
				.fileSize = fileSize,
				.header = result,
			});
		}
		return result;
	}

	PluginFile PluginParser::Parse(
		const LoadOrderEntry& a_entry,
		std::size_t a_sourceFileIndex,
		std::span<const LoadOrderEntry> a_loadOrder,
		ParseMode a_mode) const
	{
		PluginFile plugin{};
		plugin.entry = a_entry;
		plugin.bytes = ReadFileBytes(a_entry.path);
		const auto bytes = std::span<const std::uint8_t>(plugin.bytes);
		if (bytes.size() < 24) {
			throw std::runtime_error("plugin is too small: " + a_entry.path.string());
		}

		const auto tes4 = ReadRecordHeader(bytes, 0);
		if (tes4.signature != SIG_TES4) {
			throw std::runtime_error("plugin does not start with TES4 header: " + a_entry.path.string());
		}
		plugin.header.flags = tes4.flags;
		ParseHeaderRecordData(std::span<const std::uint8_t>(bytes).subspan(24, tes4.dataSize), plugin.header);
		plugin.records.reserve(static_cast<std::size_t>(plugin.header.recordCount) + 1);

		ParserContext context{
			.entry = plugin.entry,
			.loadOrder = a_loadOrder,
			.mode = a_mode,
			.sourceFileIndex = a_sourceFileIndex,
			.sourceMasterFileIDs = BuildSourceMasterFileIDs(plugin.entry, a_loadOrder),
		};
		if (a_mode == ParseMode::FullRaw) {
			ParseRecord(bytes, 0, plugin, context);
		}
		ParseRange(bytes, 24 + tes4.dataSize, bytes.size(), plugin, context);
		return plugin;
	}

	StaticPluginShard PluginParser::ParseStaticWorldShard(
		const LoadOrderEntry& a_entry,
		std::size_t a_sourceFileIndex,
		std::span<const LoadOrderEntry> a_loadOrder) const
	{
		const auto totalBegin = Clock::now();
		StaticPluginShard shard{};
		shard.entry = a_entry;
		const auto readBegin = Clock::now();
		auto fileBytes = MapFileBytes(a_entry.path);
		shard.stats.readMilliseconds = ElapsedMilliseconds(readBegin);
		const auto bytes = fileBytes.Span();
		shard.stats.fileBytes = bytes.size();
		if (bytes.size() < 24) {
			throw std::runtime_error("plugin is too small: " + a_entry.path.string());
		}

		const auto tes4 = ReadRecordHeader(bytes, 0);
		if (tes4.signature != SIG_TES4) {
			throw std::runtime_error("plugin does not start with TES4 header: " + a_entry.path.string());
		}
		shard.header.flags = a_entry.headerFlags != 0 ? a_entry.headerFlags : tes4.flags;
		shard.header.recordCount = a_entry.recordCount;
		if (shard.header.recordCount == 0) {
			ParseHeaderRecordData(bytes.subspan(24, tes4.dataSize), shard.header);
		}

		const auto reserveCount = static_cast<std::size_t>(shard.header.recordCount);
		shard.baseObjects.reserve(reserveCount / 12);
		shard.cells.reserve(reserveCount / 8);
		shard.placements.reserve(reserveCount / 2);
		shard.suppressors.reserve(reserveCount / 32);

		ParserContext context{
			.entry = shard.entry,
			.loadOrder = a_loadOrder,
			.mode = ParseMode::StaticWorldIndex,
			.sourceFileIndex = a_sourceFileIndex,
			.sourceMasterFileIDs = BuildSourceMasterFileIDs(shard.entry, a_loadOrder),
		};
		const auto scanBegin = Clock::now();
		ParseStaticRange(bytes, 24 + tes4.dataSize, bytes.size(), shard, context);
		shard.stats.scanMilliseconds = ElapsedMilliseconds(scanBegin);
		shard.stats.totalMilliseconds = ElapsedMilliseconds(totalBegin);
		return shard;
	}

	std::vector<LoadOrderEntry> PrepareLoadOrder(std::vector<LoadOrderEntry> a_entries)
	{
		oneapi::tbb::parallel_for(
			oneapi::tbb::blocked_range<std::size_t>(0, a_entries.size()),
			[&](const oneapi::tbb::blocked_range<std::size_t>& range) {
				PluginParser parser;
				for (std::size_t i = range.begin(); i != range.end(); ++i) {
					auto& entry = a_entries[i];
					entry.pluginName = entry.pluginName.empty() ? PluginNameFromPath(entry.path) : LowerAscii(entry.pluginName);
					entry.masters.clear();
					const auto header = parser.ReadHeader(entry.path);
					entry.headerFlags = header.flags;
					entry.recordCount = header.recordCount;
					entry.kind = DetectModuleKind(entry.path, header);
					entry.masters.reserve(header.masters.size());
					for (auto master : header.masters) {
						entry.masters.push_back(LowerAscii(master));
					}
				}
			});

		std::uint16_t fullSlot = 0;
		std::uint16_t lightSlot = 0;
		for (auto& entry : a_entries) {
			if (entry.kind == ModuleKind::Light) {
				if (lightSlot > 0x0FFFu) {
					throw std::runtime_error("too many ESL/light plugins");
				}
				entry.fileID = FileID{ .kind = ModuleKind::Light, .slot = lightSlot++ };
			} else {
				if (fullSlot > 0xFDu) {
					throw std::runtime_error("too many full plugins");
				}
				entry.fileID = FileID{ .kind = ModuleKind::Full, .slot = fullSlot++ };
			}
		}
		return a_entries;
	}

	ResolvedRecordStore BuildResolvedRecordStore(const std::vector<PluginFile>& a_plugins)
	{
		ResolvedRecordStore store;
		std::size_t recordCount = 0;
		for (const auto& plugin : a_plugins) {
			recordCount += plugin.records.size();
		}
		store.chains.reserve(recordCount);
		for (const auto& plugin : a_plugins) {
			for (const auto& record : plugin.records) {
				if (record.loadOrderFormID.IsNull() || record.loadOrderFormID.IsNone()) {
					continue;
				}
				auto [it, inserted] = store.chains.try_emplace(record.loadOrderFormID);
				auto& chain = it->second;
				if (inserted) {
					chain.formID = record.loadOrderFormID;
					chain.firstRecord = std::addressof(record);
				} else {
					chain.overrides.push_back(std::addressof(record));
				}
			}
		}
		return store;
	}

	namespace
	{
		struct LocalIndexRecords
		{
			std::vector<BaseObjectInfo> bases;
			std::vector<TextureSetInfo> textureSets;
			std::vector<LandTextureInfo> landTextures;
			std::vector<GrassInfo> grasses;
			std::vector<CellInfo> cells;
			std::vector<LandInfo> lands;
			std::vector<const RawRecord*> placementRecords;
		};

		struct LocalPlacementBuckets
		{
			std::vector<std::pair<CellKey, PlacementInfo>> exterior;
			std::vector<std::pair<FormID, PlacementInfo>> interior;
			std::vector<PlacementInfo> orphan;
		};

		struct LocalStaticPlacementBuckets
		{
			std::vector<std::pair<CellKey, PlacementInfo>> exterior;
			std::vector<std::pair<FormID, PlacementInfo>> interior;
			std::vector<PlacementInfo> orphan;
			std::size_t winning{ 0 };
			std::size_t disabled{ 0 };
			std::size_t missingBase{ 0 };
			std::size_t missingCell{ 0 };
		};

		[[nodiscard]] std::int32_t WorldCoordinateToCell(float a_coordinate)
		{
			return static_cast<std::int32_t>(std::floor(a_coordinate / kSkyrimTerrainCellSize));
		}

		[[nodiscard]] std::optional<CellKey> ResolvePlacementExteriorCellKey(
			const CellInfo& a_cell,
			const PlacementInfo& a_placement)
		{
			if (!a_cell.worldFormID || a_cell.IsInterior()) {
				return std::nullopt;
			}
			if (a_cell.gridX && a_cell.gridY) {
				return CellKey{ *a_cell.worldFormID, *a_cell.gridX, *a_cell.gridY };
			}
			return CellKey{
				*a_cell.worldFormID,
				WorldCoordinateToCell(a_placement.position[0]),
				WorldCoordinateToCell(a_placement.position[1])
			};
		}

		class FormIDSeenSet
		{
		public:
			explicit FormIDSeenSet(std::size_t a_expectedCount)
			{
				std::size_t capacity = 1;
				const auto target = (std::max<std::size_t>)(16, a_expectedCount * 2);
				while (capacity < target) {
					capacity <<= 1;
				}
				_slots.assign(capacity, 0);
				_mask = capacity - 1;
			}

			[[nodiscard]] bool Insert(FormID a_formID)
			{
				auto value = a_formID.value;
				if (value == 0) {
					return false;
				}

				auto index = Hash(value) & _mask;
				for (;;) {
					auto& slot = _slots[index];
					if (slot == value) {
						return false;
					}
					if (slot == 0) {
						slot = value;
						return true;
					}
					index = (index + 1) & _mask;
				}
			}

		private:
			[[nodiscard]] static std::size_t Hash(std::uint32_t a_value) noexcept
			{
				auto value = a_value;
				value ^= value >> 16;
				value *= 0x7feb352du;
				value ^= value >> 15;
				value *= 0x846ca68bu;
				value ^= value >> 16;
				return value;
			}

			std::vector<std::uint32_t> _slots;
			std::size_t _mask{ 0 };
		};

		[[nodiscard]] WorldIndex BuildWorldIndexFromWinningRecords(std::span<const RawRecord* const> a_winningRecords)
		{
			WorldIndex index;
			index.baseObjects.reserve(24000);
			index.textureSets.reserve(1024);
			index.landTextures.reserve(2048);
			index.grasses.reserve(2048);
			index.cells.reserve(90000);

			oneapi::tbb::enumerable_thread_specific<LocalIndexRecords> localRecords;
			oneapi::tbb::parallel_for(
				oneapi::tbb::blocked_range<std::size_t>(0, a_winningRecords.size(), 4096),
				[&](const oneapi::tbb::blocked_range<std::size_t>& range) {
					auto& local = localRecords.local();
					for (std::size_t i = range.begin(); i != range.end(); ++i) {
						const auto* record = a_winningRecords[i];
						if (!record) {
							continue;
						}
						if (record->signature == SIG_REFR || record->signature == SIG_ACHR) {
							local.placementRecords.push_back(record);
						} else if (record->signature == SIG_CELL) {
							if (auto cell = ExtractCell(*record)) {
								local.cells.push_back(std::move(*cell));
							}
						} else if (record->signature == SIG_LAND) {
							if (auto land = ExtractLand(*record)) {
								local.lands.push_back(std::move(*land));
							}
						} else if (IsBaseObjectSignature(record->signature)) {
							if (auto base = ExtractBaseObject(*record)) {
								local.bases.push_back(std::move(*base));
							}
						} else if (record->signature == SIG_TXST) {
							if (auto textureSet = ExtractTextureSet(*record)) {
								local.textureSets.push_back(std::move(*textureSet));
							}
						} else if (record->signature == SIG_LTEX) {
							if (auto landTexture = ExtractLandTexture(*record)) {
								local.landTextures.push_back(std::move(*landTexture));
							}
						} else if (record->signature == SIG_GRAS) {
							if (auto grass = ExtractGrass(*record)) {
								local.grasses.push_back(std::move(*grass));
							}
						}
					}
				});

			std::vector<const RawRecord*> placementRecords;
			std::vector<LandInfo> lands;
			for (auto& local : localRecords) {
				for (auto& base : local.bases) {
					index.baseObjects[base.formID] = std::move(base);
				}
				for (auto& textureSet : local.textureSets) {
					index.textureSets[textureSet.formID] = std::move(textureSet);
				}
				for (auto& landTexture : local.landTextures) {
					index.landTextures[landTexture.formID] = std::move(landTexture);
				}
				for (auto& grass : local.grasses) {
					index.grasses[grass.formID] = std::move(grass);
				}
				for (auto& cell : local.cells) {
					index.cells[cell.formID] = std::move(cell);
				}
				lands.insert(lands.end(), std::make_move_iterator(local.lands.begin()), std::make_move_iterator(local.lands.end()));
				placementRecords.insert(placementRecords.end(), local.placementRecords.begin(), local.placementRecords.end());
			}

			for (auto& land : lands) {
				const auto cellIt = index.cells.find(land.parentCell);
				if (cellIt == index.cells.end() || !cellIt->second.worldFormID || !cellIt->second.gridX || !cellIt->second.gridY) {
					continue;
				}
				land.worldFormID = cellIt->second.worldFormID;
				land.cellX = cellIt->second.gridX;
				land.cellY = cellIt->second.gridY;
				index.landsByWorldspace[*land.worldFormID].push_back(std::move(land));
			}

			oneapi::tbb::enumerable_thread_specific<LocalPlacementBuckets> localBuckets;
			oneapi::tbb::parallel_for(
				oneapi::tbb::blocked_range<std::size_t>(0, placementRecords.size(), 4096),
				[&](const oneapi::tbb::blocked_range<std::size_t>& range) {
					auto& local = localBuckets.local();
					for (std::size_t i = range.begin(); i != range.end(); ++i) {
						const auto* record = placementRecords[i];
						auto placement = ExtractPlacement(*record);
						if (!placement || placement->IsInitiallyDisabled()) {
							continue;
						}
						if (!index.baseObjects.contains(placement->baseFormID)) {
							continue;
						}
						const auto cellIt = index.cells.find(placement->parentCell);
						if (cellIt == index.cells.end()) {
							local.orphan.push_back(std::move(*placement));
							continue;
						}
						const auto& cell = cellIt->second;
						placement->worldFormID = cell.worldFormID;
						if (auto exteriorKey = ResolvePlacementExteriorCellKey(cell, *placement)) {
							local.exterior.emplace_back(*exteriorKey, std::move(*placement));
						} else {
							local.interior.emplace_back(cell.formID, std::move(*placement));
						}
					}
				});

			for (auto& local : localBuckets) {
				for (auto& [cellKey, placement] : local.exterior) {
					index.exteriorPlacements[cellKey].push_back(std::move(placement));
				}
				for (auto& [cellFormID, placement] : local.interior) {
					index.interiorPlacements[cellFormID].push_back(std::move(placement));
				}
				index.orphanPlacements.insert(index.orphanPlacements.end(), std::make_move_iterator(local.orphan.begin()), std::make_move_iterator(local.orphan.end()));
			}
			return index;
		}
	}

	WorldIndex BuildWorldIndex(const ResolvedRecordStore& a_resolvedRecords)
	{
		std::vector<const RawRecord*> winningRecords;
		winningRecords.reserve(a_resolvedRecords.chains.size());
		for (const auto& [_, chain] : a_resolvedRecords.chains) {
			winningRecords.push_back(chain.Winning());
		}
		return BuildWorldIndexFromWinningRecords(winningRecords);
	}

	WorldIndex BuildStaticWorldIndex(const std::vector<PluginFile>& a_plugins)
	{
		std::size_t recordCount = 0;
		for (const auto& plugin : a_plugins) {
			recordCount += plugin.records.size();
		}

		std::unordered_map<FormID, const RawRecord*, FormIDHash> winning;
		winning.reserve(recordCount);
		for (const auto& plugin : a_plugins) {
			for (const auto& record : plugin.records) {
				if (record.loadOrderFormID.IsNull() || record.loadOrderFormID.IsNone()) {
					continue;
				}
				winning.insert_or_assign(record.loadOrderFormID, std::addressof(record));
			}
		}

		std::vector<const RawRecord*> winningRecords;
		winningRecords.reserve(winning.size());
		for (const auto& [_, record] : winning) {
			winningRecords.push_back(record);
		}
		return BuildWorldIndexFromWinningRecords(winningRecords);
	}

	std::array<float, LandInfo::VertexCount> DecodeLandHeights(std::span<const std::uint8_t> a_vhgtPayload)
	{
		return DecodeLandHeightsInternal(a_vhgtPayload);
	}

	const std::vector<PlacementInfo>* StaticWorldSnapshot::GetExteriorCell(FormID a_worldFormID, std::int32_t a_x, std::int32_t a_y) const
	{
		const auto it = exteriorPlacementsByCell.find(CellKey{ a_worldFormID, a_x, a_y });
		return it == exteriorPlacementsByCell.end() ? nullptr : std::addressof(it->second);
	}

	const std::vector<PlacementInfo>* StaticWorldSnapshot::GetInteriorCell(FormID a_cellFormID) const
	{
		const auto it = interiorPlacementsByCell.find(a_cellFormID);
		return it == interiorPlacementsByCell.end() ? nullptr : std::addressof(it->second);
	}

	WorldIndex StaticWorldSnapshot::MaterializeWorldIndex() const
	{
		WorldIndex index;
		index.baseObjects = baseObjectsByFormID;
		index.textureSets = textureSetsByFormID;
		index.landTextures = landTexturesByFormID;
		index.grasses = grassesByFormID;
		index.cells = cellsByFormID;
		index.landsByWorldspace = landsByWorldspace;
		index.exteriorPlacements = exteriorPlacementsByCell;
		index.interiorPlacements = interiorPlacementsByCell;
		index.orphanPlacements = orphanPlacements;
		return index;
	}

	StaticWorldSnapshot BuildStaticWorldSnapshot(
		const std::vector<StaticPluginShard>& a_shards,
		StaticWorldSnapshotBuildOptions a_options)
	{
		StaticWorldSnapshot snapshot;

		auto phaseBegin = Clock::now();
		std::size_t baseCount = 0;
		std::size_t textureSetCount = 0;
		std::size_t landTextureCount = 0;
		std::size_t grassCount = 0;
		std::size_t worldCount = 0;
		std::size_t cellCount = 0;
		std::size_t landCount = 0;
		std::size_t placementCount = 0;
		std::size_t suppressorCount = 0;
		for (const auto& shard : a_shards) {
			baseCount += shard.baseObjects.size();
			textureSetCount += shard.textureSets.size();
			landTextureCount += shard.landTextures.size();
			grassCount += shard.grasses.size();
			worldCount += shard.worlds.size();
			cellCount += shard.cells.size();
			landCount += shard.lands.size();
			placementCount += shard.placements.size();
			suppressorCount += shard.suppressors.size();
		}
		snapshot.buildStats.countInputsMilliseconds = ElapsedMilliseconds(phaseBegin);
		snapshot.buildStats.placementCandidates = placementCount;

		phaseBegin = Clock::now();
		snapshot.baseObjectsByFormID.reserve(baseCount);
		snapshot.textureSetsByFormID.reserve(textureSetCount);
		snapshot.landTexturesByFormID.reserve(landTextureCount);
		snapshot.grassesByFormID.reserve(grassCount);
		snapshot.worldsByFormID.reserve(worldCount);
		snapshot.cellsByFormID.reserve(cellCount);

		for (const auto& shard : a_shards) {
			for (const auto& world : shard.worlds) {
				if (!world.formID.IsNull() && !world.formID.IsNone()) {
					snapshot.worldsByFormID.insert_or_assign(world.formID, world);
				}
			}
			for (const auto& base : shard.baseObjects) {
				if (!base.formID.IsNull() && !base.formID.IsNone()) {
					snapshot.baseObjectsByFormID.insert_or_assign(base.formID, base);
				}
			}
			for (const auto& textureSet : shard.textureSets) {
				if (!textureSet.formID.IsNull() && !textureSet.formID.IsNone()) {
					snapshot.textureSetsByFormID.insert_or_assign(textureSet.formID, textureSet);
				}
			}
			for (const auto& landTexture : shard.landTextures) {
				if (!landTexture.formID.IsNull() && !landTexture.formID.IsNone()) {
					snapshot.landTexturesByFormID.insert_or_assign(landTexture.formID, landTexture);
				}
			}
			for (const auto& grass : shard.grasses) {
				if (!grass.formID.IsNull() && !grass.formID.IsNone()) {
					snapshot.grassesByFormID.insert_or_assign(grass.formID, grass);
				}
			}
			for (const auto& cell : shard.cells) {
				if (!cell.formID.IsNull() && !cell.formID.IsNone()) {
					snapshot.cellsByFormID.insert_or_assign(cell.formID, cell);
				}
			}
			for (const auto& suppressor : shard.suppressors) {
				++snapshot.suppressedRecords;
				if (IsBaseObjectSignature(suppressor.signature)) {
					snapshot.baseObjectsByFormID.erase(suppressor.formID);
				} else if (suppressor.signature == SIG_TXST) {
					snapshot.textureSetsByFormID.erase(suppressor.formID);
				} else if (suppressor.signature == SIG_LTEX) {
					snapshot.landTexturesByFormID.erase(suppressor.formID);
				} else if (suppressor.signature == SIG_GRAS) {
					snapshot.grassesByFormID.erase(suppressor.formID);
				} else if (suppressor.signature == SIG_WRLD) {
					snapshot.worldsByFormID.erase(suppressor.formID);
				} else if (suppressor.signature == SIG_CELL) {
					snapshot.cellsByFormID.erase(suppressor.formID);
				}
			}
		}

		// A LandInfo is ~11 KB of fixed arrays, so a move is a copy. Select the
		// winners first, then size each worldspace once and copy every winner
		// exactly once instead of copying rejected records and regrowing vectors.
		FormIDSeenSet seenLands{ landCount + suppressorCount };
		std::vector<std::pair<const LandInfo*, const CellInfo*>> winningLands;
		winningLands.reserve(landCount);
		std::unordered_map<FormID, std::size_t, FormIDHash> landCountsByWorld;
		for (auto shardIt = a_shards.rbegin(); shardIt != a_shards.rend(); ++shardIt) {
			for (const auto& suppressor : shardIt->suppressors) {
				if (suppressor.signature == SIG_LAND) {
					(void)seenLands.Insert(suppressor.formID);
				}
			}
			for (auto landIt = shardIt->lands.rbegin(); landIt != shardIt->lands.rend(); ++landIt) {
				if (landIt->formID.IsNull() || landIt->formID.IsNone() || !seenLands.Insert(landIt->formID)) {
					continue;
				}
				const auto cellIt = snapshot.cellsByFormID.find(landIt->parentCell);
				if (cellIt == snapshot.cellsByFormID.end() || !cellIt->second.worldFormID || !cellIt->second.gridX || !cellIt->second.gridY) {
					continue;
				}
				winningLands.emplace_back(std::addressof(*landIt), std::addressof(cellIt->second));
				++landCountsByWorld[*cellIt->second.worldFormID];
			}
		}
		for (const auto& [worldFormID, count] : landCountsByWorld) {
			snapshot.landsByWorldspace[worldFormID].reserve(count);
		}
		for (const auto& [source, cell] : winningLands) {
			auto& land = snapshot.landsByWorldspace[*cell->worldFormID].emplace_back(*source);
			land.worldFormID = cell->worldFormID;
			land.cellX = cell->gridX;
			land.cellY = cell->gridY;
		}
		snapshot.buildStats.mergeRecordsMilliseconds = ElapsedMilliseconds(phaseBegin);

		phaseBegin = Clock::now();
		FormIDSeenSet seenPlacements{ placementCount + suppressorCount };
		snapshot.exteriorPlacementsByCell.reserve(16000);
		snapshot.interiorPlacementsByCell.reserve(4096);

		const auto bucketPlacement = [&](PlacementInfo placement) {
			if (placement.IsInitiallyDisabled()) {
				++snapshot.buildStats.skippedDisabledPlacements;
				return;
			}
			if (!snapshot.baseObjectsByFormID.contains(placement.baseFormID)) {
				++snapshot.buildStats.skippedMissingBasePlacements;
				return;
			}
			const auto cellIt = snapshot.cellsByFormID.find(placement.parentCell);
			if (cellIt == snapshot.cellsByFormID.end()) {
				++snapshot.buildStats.skippedMissingCellPlacements;
				snapshot.orphanPlacements.push_back(std::move(placement));
				return;
			}

			const auto& cell = cellIt->second;
			placement.worldFormID = cell.worldFormID;
			if (auto exteriorKey = ResolvePlacementExteriorCellKey(cell, placement)) {
				snapshot.exteriorPlacementsByCell[*exteriorKey].push_back(std::move(placement));
			} else {
				snapshot.interiorPlacementsByCell[cell.formID].push_back(std::move(placement));
			}
			++snapshot.buildStats.winningPlacements;
		};

		if (a_options.parallelPlacementBucketing) {
			std::vector<const PlacementInfo*> winningPlacements;
			winningPlacements.reserve(placementCount);
			for (auto shardIt = a_shards.rbegin(); shardIt != a_shards.rend(); ++shardIt) {
				for (const auto& suppressor : shardIt->suppressors) {
					if (suppressor.signature != SIG_REFR && suppressor.signature != SIG_ACHR) {
						continue;
					}
					if (suppressor.formID.IsNull() || suppressor.formID.IsNone()) {
						continue;
					}
					if (seenPlacements.Insert(suppressor.formID)) {
						++snapshot.buildStats.skippedDeletedPlacements;
					}
				}

				for (auto placementIt = shardIt->placements.rbegin(); placementIt != shardIt->placements.rend(); ++placementIt) {
					if (placementIt->formID.IsNull() || placementIt->formID.IsNone()) {
						continue;
					}
					if (!seenPlacements.Insert(placementIt->formID)) {
						++snapshot.buildStats.skippedPlacementOverrides;
						continue;
					}
					winningPlacements.push_back(std::addressof(*placementIt));
				}
			}

			oneapi::tbb::enumerable_thread_specific<LocalStaticPlacementBuckets> localBuckets;
			oneapi::tbb::parallel_for(
				oneapi::tbb::blocked_range<std::size_t>(0, winningPlacements.size(), 4096),
				[&](const oneapi::tbb::blocked_range<std::size_t>& range) {
					auto& local = localBuckets.local();
					for (std::size_t i = range.begin(); i != range.end(); ++i) {
						PlacementInfo placement = *winningPlacements[i];
						if (placement.IsInitiallyDisabled()) {
							++local.disabled;
							continue;
						}
						if (!snapshot.baseObjectsByFormID.contains(placement.baseFormID)) {
							++local.missingBase;
							continue;
						}
						const auto cellIt = snapshot.cellsByFormID.find(placement.parentCell);
						if (cellIt == snapshot.cellsByFormID.end()) {
							++local.missingCell;
							local.orphan.push_back(std::move(placement));
							continue;
						}

						const auto& cell = cellIt->second;
						placement.worldFormID = cell.worldFormID;
						if (auto exteriorKey = ResolvePlacementExteriorCellKey(cell, placement)) {
							local.exterior.emplace_back(*exteriorKey, std::move(placement));
						} else {
							local.interior.emplace_back(cell.formID, std::move(placement));
						}
						++local.winning;
					}
				});

			for (auto& local : localBuckets) {
				snapshot.buildStats.winningPlacements += local.winning;
				snapshot.buildStats.skippedDisabledPlacements += local.disabled;
				snapshot.buildStats.skippedMissingBasePlacements += local.missingBase;
				snapshot.buildStats.skippedMissingCellPlacements += local.missingCell;
				for (auto& [cellKey, placement] : local.exterior) {
					snapshot.exteriorPlacementsByCell[cellKey].push_back(std::move(placement));
				}
				for (auto& [cellFormID, placement] : local.interior) {
					snapshot.interiorPlacementsByCell[cellFormID].push_back(std::move(placement));
				}
				snapshot.orphanPlacements.insert(
					snapshot.orphanPlacements.end(),
					std::make_move_iterator(local.orphan.begin()),
					std::make_move_iterator(local.orphan.end()));
			}
		} else {
			for (auto shardIt = a_shards.rbegin(); shardIt != a_shards.rend(); ++shardIt) {
				for (const auto& suppressor : shardIt->suppressors) {
					if (suppressor.signature != SIG_REFR && suppressor.signature != SIG_ACHR) {
						continue;
					}
					if (suppressor.formID.IsNull() || suppressor.formID.IsNone()) {
						continue;
					}
					if (seenPlacements.Insert(suppressor.formID)) {
						++snapshot.buildStats.skippedDeletedPlacements;
					}
				}

				for (auto placementIt = shardIt->placements.rbegin(); placementIt != shardIt->placements.rend(); ++placementIt) {
					if (placementIt->formID.IsNull() || placementIt->formID.IsNone()) {
						continue;
					}
					if (!seenPlacements.Insert(placementIt->formID)) {
						++snapshot.buildStats.skippedPlacementOverrides;
						continue;
					}
					bucketPlacement(*placementIt);
				}
			}
		}
		snapshot.buildStats.bucketPlacementsMilliseconds = ElapsedMilliseconds(phaseBegin);

		phaseBegin = Clock::now();
		snapshot.exteriorCellKeys.reserve(snapshot.exteriorPlacementsByCell.size());
		for (const auto& [key, _] : snapshot.exteriorPlacementsByCell) {
			snapshot.exteriorCellKeys.push_back(key);
		}
		std::ranges::sort(snapshot.exteriorCellKeys, [](const CellKey& lhs, const CellKey& rhs) {
			if (lhs.worldFormID != rhs.worldFormID) {
				return lhs.worldFormID < rhs.worldFormID;
			}
			if (lhs.x != rhs.x) {
				return lhs.x < rhs.x;
			}
			return lhs.y < rhs.y;
		});

		snapshot.interiorCellKeys.reserve(snapshot.interiorPlacementsByCell.size());
		for (const auto& [cell, _] : snapshot.interiorPlacementsByCell) {
			snapshot.interiorCellKeys.push_back(cell);
		}
		std::ranges::sort(snapshot.interiorCellKeys);
		snapshot.buildStats.publishKeysMilliseconds = ElapsedMilliseconds(phaseBegin);
		return snapshot;
	}

	GameDataSnapshot LoadGameData(ILoadOrderSource& a_source)
	{
		auto loadOrder = PrepareLoadOrder(a_source.Load());
		PluginParser parser;
		GameDataSnapshot snapshot;
		snapshot.plugins.reserve(loadOrder.size());
		for (std::size_t i = 0; i < loadOrder.size(); ++i) {
			snapshot.plugins.push_back(parser.Parse(loadOrder[i], i, loadOrder));
		}
		snapshot.resolvedRecords = BuildResolvedRecordStore(snapshot.plugins);
		snapshot.worldIndex = BuildWorldIndex(snapshot.resolvedRecords);
		return snapshot;
	}
}
