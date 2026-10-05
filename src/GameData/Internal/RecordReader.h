#pragma once

#include "GameData/LoadOrder.h"

#include <zlib.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// The plugin file format: record, group and subrecord headers, compressed records, and form IDs.
namespace FasterNGIO::GameData::Internal
{
	inline constexpr FourCC kSigTes4 = MakeFourCC('T', 'E', 'S', '4');
	inline constexpr FourCC kSigGrup = MakeFourCC('G', 'R', 'U', 'P');
	inline constexpr FourCC kSigXxxx = MakeFourCC('X', 'X', 'X', 'X');
	inline constexpr FourCC kSigEdid = MakeFourCC('E', 'D', 'I', 'D');
	inline constexpr FourCC kSigMast = MakeFourCC('M', 'A', 'S', 'T');
	inline constexpr FourCC kSigHedr = MakeFourCC('H', 'E', 'D', 'R');
	inline constexpr FourCC kSigModl = MakeFourCC('M', 'O', 'D', 'L');
	inline constexpr FourCC kSigObnd = MakeFourCC('O', 'B', 'N', 'D');
	inline constexpr FourCC kSigName = MakeFourCC('N', 'A', 'M', 'E');
	inline constexpr FourCC kSigData = MakeFourCC('D', 'A', 'T', 'A');
	inline constexpr FourCC kSigXclc = MakeFourCC('X', 'C', 'L', 'C');
	inline constexpr FourCC kSigXclw = MakeFourCC('X', 'C', 'L', 'W');
	inline constexpr FourCC kSigXscl = MakeFourCC('X', 'S', 'C', 'L');
	inline constexpr FourCC kSigGnam = MakeFourCC('G', 'N', 'A', 'M');
	inline constexpr FourCC kSigVhgt = MakeFourCC('V', 'H', 'G', 'T');
	inline constexpr FourCC kSigVclr = MakeFourCC('V', 'C', 'L', 'R');
	inline constexpr FourCC kSigBtxt = MakeFourCC('B', 'T', 'X', 'T');
	inline constexpr FourCC kSigAtxt = MakeFourCC('A', 'T', 'X', 'T');
	inline constexpr FourCC kSigVtxt = MakeFourCC('V', 'T', 'X', 'T');

	inline constexpr FourCC kSigStat = MakeFourCC('S', 'T', 'A', 'T');
	inline constexpr FourCC kSigMstt = MakeFourCC('M', 'S', 'T', 'T');
	inline constexpr FourCC kSigTree = MakeFourCC('T', 'R', 'E', 'E');
	inline constexpr FourCC kSigActi = MakeFourCC('A', 'C', 'T', 'I');
	inline constexpr FourCC kSigDoor = MakeFourCC('D', 'O', 'O', 'R');
	inline constexpr FourCC kSigCont = MakeFourCC('C', 'O', 'N', 'T');
	inline constexpr FourCC kSigFurn = MakeFourCC('F', 'U', 'R', 'N');
	inline constexpr FourCC kSigLigh = MakeFourCC('L', 'I', 'G', 'H');
	inline constexpr FourCC kSigFlor = MakeFourCC('F', 'L', 'O', 'R');
	inline constexpr FourCC kSigScol = MakeFourCC('S', 'C', 'O', 'L');
	inline constexpr FourCC kSigTact = MakeFourCC('T', 'A', 'C', 'T');
	inline constexpr FourCC kSigWrld = MakeFourCC('W', 'R', 'L', 'D');
	inline constexpr FourCC kSigCell = MakeFourCC('C', 'E', 'L', 'L');
	inline constexpr FourCC kSigRefr = MakeFourCC('R', 'E', 'F', 'R');
	inline constexpr FourCC kSigAchr = MakeFourCC('A', 'C', 'H', 'R');
	inline constexpr FourCC kSigLtex = MakeFourCC('L', 'T', 'E', 'X');
	inline constexpr FourCC kSigGras = MakeFourCC('G', 'R', 'A', 'S');
	inline constexpr FourCC kSigLand = MakeFourCC('L', 'A', 'N', 'D');

	inline constexpr std::size_t kRecordHeaderSize = 24;
	inline constexpr std::uint32_t kRecordFlagDeleted = 1u << 5;
	inline constexpr std::uint32_t kRecordFlagIgnored = 1u << 12;
	inline constexpr std::uint32_t kRecordFlagCompressed = 1u << 18;
	inline constexpr std::uint32_t kTes4FlagLight = 1u << 9;

	// The base objects a reference can place: the record types FasterNGIO keeps models of.
	[[nodiscard]] constexpr bool IsBaseObjectSignature(FourCC a_signature)
	{
		return a_signature == kSigStat || a_signature == kSigMstt || a_signature == kSigTree || a_signature == kSigActi || a_signature == kSigDoor ||
		       a_signature == kSigCont || a_signature == kSigFurn || a_signature == kSigLigh || a_signature == kSigFlor || a_signature == kSigScol ||
		       a_signature == kSigTact;
	}

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

	// A zero-terminated string field (the whole payload when it has no terminator).
	[[nodiscard]] std::string ReadString(std::span<const std::uint8_t> a_data);

	struct RecordHeader
	{
		FourCC signature{ 0 };
		std::uint32_t dataSize{ 0 };
		std::uint32_t flags{ 0 };
		// As stored in the plugin: plugin-local.
		FormID formID{};

		[[nodiscard]] bool IsSuppressed() const { return (flags & (kRecordFlagDeleted | kRecordFlagIgnored)) != 0; }
		[[nodiscard]] bool IsCompressed() const { return (flags & kRecordFlagCompressed) != 0; }
	};

	[[nodiscard]] RecordHeader ReadRecordHeader(std::span<const std::uint8_t> a_bytes, std::size_t a_offset);

	struct GroupHeader
	{
		std::uint32_t groupSize{ 0 };
		std::uint32_t label{ 0 };
		std::int32_t type{ 0 };
	};

	[[nodiscard]] GroupHeader ReadGroupHeader(std::span<const std::uint8_t> a_bytes, std::size_t a_offset);

	// A subrecord header. An XXXX subrecord carries the next one's 32-bit size, so the pair reads
	// as one header of 16 bytes.
	struct SubrecordHeader
	{
		FourCC signature{ 0 };
		std::uint32_t size{ 0 };
		std::size_t headerSize{ 0 };
	};

	// The header at a_offset, or nullopt when a_data ends before it does.
	[[nodiscard]] std::optional<SubrecordHeader> PeekSubrecordHeader(std::span<const std::uint8_t> a_data, std::size_t a_offset);

	struct Subrecord
	{
		FourCC signature{ 0 };
		std::span<const std::uint8_t> payload;
	};

	// Walks a record's subrecords; stops at the first one that does not fit.
	class SubrecordCursor
	{
	public:
		explicit SubrecordCursor(std::span<const std::uint8_t> a_data) :
			_data(a_data)
		{}

		[[nodiscard]] bool Next(Subrecord& a_subrecord);

	private:
		std::span<const std::uint8_t> _data;
		std::size_t _offset{ 0 };
	};

	// Inflates a compressed record's data (a 32-bit uncompressed size, then a zlib stream) into
	// a_output.
	void InflateRecord(std::span<const std::uint8_t> a_recordData, std::vector<std::uint8_t>& a_output);

	// A reusable zlib stream for inflating a compressed record piece by piece.
	class InflateStream
	{
	public:
		InflateStream() = default;
		InflateStream(const InflateStream&) = delete;
		InflateStream& operator=(const InflateStream&) = delete;
		~InflateStream();

		// Starts on a compressed record's data.
		void Reset(std::span<const std::uint8_t> a_recordData);
		// Inflates into a_output; returns the bytes produced (0 at the end).
		[[nodiscard]] std::size_t Read(std::span<std::uint8_t> a_output);

	private:
		z_stream _stream{};
		bool _initialized{ false };
		bool _ended{ false };
	};

	// Converts a plugin's local form IDs to load-order IDs: the high byte indexes the plugin's
	// masters, and anything past them is the plugin itself.
	class LocalFormIDs
	{
	public:
		// Throws when a master is not in a_loadOrder.
		LocalFormIDs(const LoadOrderEntry& a_plugin, std::span<const LoadOrderEntry> a_loadOrder);

		[[nodiscard]] FormID Resolve(FormID a_local) const;

	private:
		FileID _self;
		std::vector<FileID> _masters;
	};

	// The TES4 header fields the load order needs.
	struct PluginHeader
	{
		std::uint32_t flags{ 0 };
		std::uint32_t recordCount{ 0 };
		std::vector<std::string> masters;
	};

	// Reads only a_path's TES4 header.
	[[nodiscard]] PluginHeader ReadPluginHeader(const std::filesystem::path& a_path);

	// a_bytes' TES4 record header (the whole file must start with one).
	[[nodiscard]] RecordHeader ReadTes4Header(std::span<const std::uint8_t> a_bytes, const std::filesystem::path& a_path);
}
