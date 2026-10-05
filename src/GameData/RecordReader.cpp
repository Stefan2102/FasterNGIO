#include "GameData/Internal/RecordReader.h"

#include "Platform/Text.h"

#include <algorithm>
#include <array>
#include <fstream>

namespace FasterNGIO::GameData::Internal
{
	std::string ReadString(std::span<const std::uint8_t> a_data)
	{
		const auto* begin = reinterpret_cast<const char*>(a_data.data());
		const auto* end = begin + a_data.size();
		return std::string(begin, std::find(begin, end, '\0'));
	}

	RecordHeader ReadRecordHeader(std::span<const std::uint8_t> a_bytes, std::size_t a_offset)
	{
		return RecordHeader{
			.signature = ReadLE<FourCC>(a_bytes, a_offset + 0),
			.dataSize = ReadLE<std::uint32_t>(a_bytes, a_offset + 4),
			.flags = ReadLE<std::uint32_t>(a_bytes, a_offset + 8),
			.formID = FormID{ ReadLE<std::uint32_t>(a_bytes, a_offset + 12) },
		};
	}

	GroupHeader ReadGroupHeader(std::span<const std::uint8_t> a_bytes, std::size_t a_offset)
	{
		return GroupHeader{
			.groupSize = ReadLE<std::uint32_t>(a_bytes, a_offset + 4),
			.label = ReadLE<std::uint32_t>(a_bytes, a_offset + 8),
			.type = ReadLE<std::int32_t>(a_bytes, a_offset + 12),
		};
	}

	std::optional<SubrecordHeader> PeekSubrecordHeader(std::span<const std::uint8_t> a_data, std::size_t a_offset)
	{
		if (a_offset + 6 > a_data.size()) {
			return std::nullopt;
		}
		const auto signature = ReadLE<FourCC>(a_data, a_offset);
		if (signature != kSigXxxx) {
			return SubrecordHeader{ .signature = signature, .size = ReadLE<std::uint16_t>(a_data, a_offset + 4), .headerSize = 6 };
		}
		// XXXX (6 bytes), its 32-bit size, then the real header, whose 16-bit size is ignored.
		if (a_offset + 16 > a_data.size()) {
			return std::nullopt;
		}
		return SubrecordHeader{ .signature = ReadLE<FourCC>(a_data, a_offset + 10), .size = ReadLE<std::uint32_t>(a_data, a_offset + 6), .headerSize = 16 };
	}

	bool SubrecordCursor::Next(Subrecord& a_subrecord)
	{
		const auto header = PeekSubrecordHeader(_data, _offset);
		if (!header || _offset + header->headerSize + header->size > _data.size()) {
			_offset = _data.size();
			return false;
		}
		a_subrecord.signature = header->signature;
		a_subrecord.payload = _data.subspan(_offset + header->headerSize, header->size);
		_offset += header->headerSize + header->size;
		return true;
	}

	void InflateRecord(std::span<const std::uint8_t> a_recordData, std::vector<std::uint8_t>& a_output)
	{
		if (a_recordData.size() < sizeof(std::uint32_t)) {
			throw std::runtime_error("compressed record is missing uncompressed size");
		}
		a_output.resize(ReadLE<std::uint32_t>(a_recordData, 0));
		uLongf outputSize = static_cast<uLongf>(a_output.size());
		const auto* compressed = reinterpret_cast<const Bytef*>(a_recordData.data() + sizeof(std::uint32_t));
		const auto compressedSize = static_cast<uLong>(a_recordData.size() - sizeof(std::uint32_t));
		const auto result = uncompress(reinterpret_cast<Bytef*>(a_output.data()), std::addressof(outputSize), compressed, compressedSize);
		if (result != Z_OK || outputSize != a_output.size()) {
			throw std::runtime_error("zlib failed to inflate compressed record");
		}
	}

	InflateStream::~InflateStream()
	{
		if (_initialized) {
			inflateEnd(std::addressof(_stream));
		}
	}

	void InflateStream::Reset(std::span<const std::uint8_t> a_recordData)
	{
		if (a_recordData.size() < sizeof(std::uint32_t)) {
			throw std::runtime_error("compressed record is missing uncompressed size");
		}
		if (!_initialized) {
			if (inflateInit(std::addressof(_stream)) != Z_OK) {
				throw std::runtime_error("zlib failed to initialize streaming inflate");
			}
			_initialized = true;
		} else if (inflateReset(std::addressof(_stream)) != Z_OK) {
			throw std::runtime_error("zlib failed to reset streaming inflate");
		}
		// zlib's input pointer is not const, but inflate never writes through it.
		_stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(a_recordData.data() + sizeof(std::uint32_t)));
		_stream.avail_in = static_cast<uInt>(a_recordData.size() - sizeof(std::uint32_t));
		_ended = false;
	}

	std::size_t InflateStream::Read(std::span<std::uint8_t> a_output)
	{
		while (!_ended) {
			_stream.next_out = reinterpret_cast<Bytef*>(a_output.data());
			_stream.avail_out = static_cast<uInt>(a_output.size());
			const auto result = inflate(std::addressof(_stream), Z_NO_FLUSH);
			if (result != Z_OK && result != Z_STREAM_END) {
				throw std::runtime_error("zlib failed to stream inflate a record");
			}
			const auto produced = a_output.size() - _stream.avail_out;
			if (result == Z_STREAM_END || (produced == 0 && _stream.avail_in == 0)) {
				_ended = true;
			}
			if (produced != 0) {
				return produced;
			}
		}
		return 0;
	}

	LocalFormIDs::LocalFormIDs(const LoadOrderEntry& a_plugin, std::span<const LoadOrderEntry> a_loadOrder) :
		_self(a_plugin.fileID)
	{
		_masters.reserve(a_plugin.masters.size());
		for (const auto& master : a_plugin.masters) {
			const auto it = std::ranges::find_if(a_loadOrder, [&](const LoadOrderEntry& a_entry) { return Platform::IEquals(a_entry.pluginName, master); });
			if (it == a_loadOrder.end()) {
				throw std::runtime_error("missing master in load order: " + master);
			}
			_masters.push_back(it->fileID);
		}
	}

	FormID LocalFormIDs::Resolve(FormID a_local) const
	{
		if (a_local.IsEmpty() || a_local.IsHardcoded()) {
			return a_local;
		}
		const auto fileSlot = static_cast<std::uint8_t>(a_local.value >> 24);
		const auto owner = fileSlot < _masters.size() ? _masters[fileSlot] : _self;
		const auto objectID = owner.kind == ModuleKind::Light ? (a_local.value & 0xFFFu) : (a_local.value & 0xFFFFFFu);
		return FormID{ owner.BaseFormID() | objectID };
	}

	PluginHeader ReadPluginHeader(const std::filesystem::path& a_path)
	{
		std::ifstream input(a_path, std::ios::binary);
		if (!input) {
			throw std::runtime_error("failed to open plugin file: " + a_path.string());
		}
		std::array<std::uint8_t, kRecordHeaderSize> headerBytes{};
		input.read(reinterpret_cast<char*>(headerBytes.data()), static_cast<std::streamsize>(headerBytes.size()));
		if (input.gcount() != static_cast<std::streamsize>(headerBytes.size())) {
			throw std::runtime_error("plugin is too small: " + a_path.string());
		}
		const auto tes4 = ReadTes4Header(headerBytes, a_path);
		std::vector<std::uint8_t> data(tes4.dataSize);
		if (!data.empty()) {
			input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
			if (input.gcount() != static_cast<std::streamsize>(data.size())) {
				throw std::runtime_error("TES4 header data exceeds file size: " + a_path.string());
			}
		}

		PluginHeader result;
		result.flags = tes4.flags;
		SubrecordCursor cursor(data);
		Subrecord subrecord;
		while (cursor.Next(subrecord)) {
			if (subrecord.signature == kSigHedr && subrecord.payload.size() >= 12) {
				result.recordCount = ReadLE<std::uint32_t>(subrecord.payload, 4);
			} else if (subrecord.signature == kSigMast) {
				result.masters.push_back(ReadString(subrecord.payload));
			}
		}
		return result;
	}

	RecordHeader ReadTes4Header(std::span<const std::uint8_t> a_bytes, const std::filesystem::path& a_path)
	{
		if (a_bytes.size() < kRecordHeaderSize) {
			throw std::runtime_error("plugin is too small: " + a_path.string());
		}
		const auto header = ReadRecordHeader(a_bytes, 0);
		if (header.signature != kSigTes4) {
			throw std::runtime_error("plugin does not start with TES4 header: " + a_path.string());
		}
		return header;
	}
}
