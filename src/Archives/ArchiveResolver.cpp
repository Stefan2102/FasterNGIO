#include "Archives/ArchiveResolver.h"

#include <lz4frame.h>
#include <spdlog/spdlog.h>
#include <zlib.h>

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <unordered_set>

namespace FasterNGIO::Archives
{
	namespace
	{
		constexpr std::uint32_t kBsaMagic = 0x00415342u;
		constexpr std::uint32_t kFlagDirectoryNames = 0x0001u;
		constexpr std::uint32_t kFlagFileNames = 0x0002u;
		constexpr std::uint32_t kFlagCompressedByDefault = 0x0004u;
		constexpr std::uint32_t kFlagEmbeddedNames = 0x0100u;
		constexpr std::uint32_t kSizeCompressionToggle = 0x40000000u;

		class ViewReader
		{
		public:
			ViewReader(const std::uint8_t* a_data, std::uint64_t a_size, std::uint64_t a_offset = 0) :
				_data(a_data), _size(a_size), _offset(a_offset) {}

			template <class T>
			[[nodiscard]] T Read()
			{
				Require(sizeof(T));
				T value{};
				std::memcpy(std::addressof(value), _data + _offset, sizeof(T));
				_offset += sizeof(T);
				return value;
			}

			[[nodiscard]] std::string_view ReadBytes(std::uint64_t a_count)
			{
				Require(a_count);
				const std::string_view bytes(reinterpret_cast<const char*>(_data + _offset), static_cast<std::size_t>(a_count));
				_offset += a_count;
				return bytes;
			}

			[[nodiscard]] std::string_view ReadZString()
			{
				const auto* begin = _data + _offset;
				const auto* end = static_cast<const std::uint8_t*>(std::memchr(begin, 0, static_cast<std::size_t>(_size - _offset)));
				if (!end) {
					throw std::runtime_error("unterminated name in BSA");
				}
				const std::string_view text(reinterpret_cast<const char*>(begin), static_cast<std::size_t>(end - begin));
				_offset += text.size() + 1;
				return text;
			}

			void Seek(std::uint64_t a_offset) { _offset = a_offset; }

		private:
			void Require(std::uint64_t a_count) const
			{
				if (_offset + a_count > _size) {
					throw std::runtime_error("unexpected end of BSA");
				}
			}

			const std::uint8_t* _data;
			std::uint64_t _size;
			std::uint64_t _offset;
		};

		[[nodiscard]] std::string LowerAscii(std::string_view a_text)
		{
			std::string result(a_text);
			std::ranges::transform(result, result.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
			return result;
		}
	}

	std::string CanonicalizeResourcePath(std::string_view a_path)
	{
		std::string result = LowerAscii(a_path);
		std::ranges::replace(result, '/', '\\');
		const auto first = result.find_first_not_of('\\');
		if (first == std::string::npos) {
			return {};
		}
		result.erase(0, first);
		if (result.starts_with("data\\")) {
			result.erase(0, 5);
		}
		return result;
	}

	BsaArchive::BsaArchive(const std::filesystem::path& a_path) :
		_path(a_path)
	{
		_file = CreateFileW(a_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
		if (_file == INVALID_HANDLE_VALUE) {
			_file = nullptr;
			throw std::runtime_error("failed to open");
		}
		LARGE_INTEGER size{};
		if (!GetFileSizeEx(_file, &size) || size.QuadPart < 36) {
			Close();
			throw std::runtime_error("file too small");
		}
		_viewSize = static_cast<std::uint64_t>(size.QuadPart);
		_mapping = CreateFileMappingW(_file, nullptr, PAGE_READONLY, 0, 0, nullptr);
		_view = _mapping ? static_cast<const std::uint8_t*>(MapViewOfFile(_mapping, FILE_MAP_READ, 0, 0, 0)) : nullptr;
		if (!_view) {
			Close();
			throw std::runtime_error("failed to map");
		}

		try {
			ViewReader header(_view, _viewSize);
			const auto magic = header.Read<std::uint32_t>();
			_version = header.Read<std::uint32_t>();
			const auto foldersOffset = header.Read<std::uint32_t>();
			_flags = header.Read<std::uint32_t>();
			const auto folderCount = header.Read<std::uint32_t>();
			const auto fileCount = header.Read<std::uint32_t>();
			(void)header.Read<std::uint32_t>();  // total folder name length
			(void)header.Read<std::uint32_t>();  // total file name length
			(void)header.Read<std::uint32_t>();  // content flags
			if (magic != kBsaMagic) {
				throw std::runtime_error("not a BSA archive");
			}
			if (_version != 103u && _version != 104u && _version != 105u) {
				throw std::runtime_error("unsupported BSA version " + std::to_string(_version));
			}
			if ((_flags & kFlagDirectoryNames) == 0 || (_flags & kFlagFileNames) == 0) {
				throw std::runtime_error("archive does not store folder/file names");
			}

			std::vector<std::uint32_t> folderFileCounts(folderCount);
			ViewReader reader(_view, _viewSize, foldersOffset);
			for (auto& count : folderFileCounts) {
				(void)reader.Read<std::uint64_t>();
				count = reader.Read<std::uint32_t>();
				if (_version == 105u) {
					(void)reader.Read<std::uint32_t>();
					(void)reader.Read<std::uint64_t>();
				} else {
					(void)reader.Read<std::uint32_t>();
				}
			}

			std::vector<std::string> folderNames(folderCount);
			std::vector<Entry> records;
			records.reserve(fileCount);
			std::vector<std::uint32_t> recordFolders;
			recordFolders.reserve(fileCount);
			for (std::uint32_t folder = 0; folder < folderCount; ++folder) {
				const auto nameLength = reader.Read<std::uint8_t>();
				auto name = reader.ReadBytes(nameLength);
				if (!name.empty() && name.back() == '\0') {
					name.remove_suffix(1);
				}
				folderNames[folder] = std::string(name);
				for (std::uint32_t i = 0; i < folderFileCounts[folder]; ++i) {
					(void)reader.Read<std::uint64_t>();
					Entry entry;
					entry.size = reader.Read<std::uint32_t>();
					entry.offset = reader.Read<std::uint32_t>();
					records.push_back(entry);
					recordFolders.push_back(folder);
				}
			}

			_entries.reserve(records.size());
			for (std::size_t i = 0; i < records.size(); ++i) {
				const auto fileName = reader.ReadZString();
				const auto& folderName = folderNames[recordFolders[i]];
				auto fullName = folderName.empty() ? std::string(fileName) : folderName + "\\" + std::string(fileName);
				auto canonical = CanonicalizeResourcePath(fullName);
				if (!canonical.empty()) {
					_entries.emplace_back(std::move(canonical), records[i]);
				}
			}
		} catch (...) {
			Close();
			throw;
		}
	}

	BsaArchive::BsaArchive(BsaArchive&& a_other) noexcept
	{
		*this = std::move(a_other);
	}

	BsaArchive& BsaArchive::operator=(BsaArchive&& a_other) noexcept
	{
		if (this != std::addressof(a_other)) {
			Close();
			_path = std::move(a_other._path);
			_file = std::exchange(a_other._file, nullptr);
			_mapping = std::exchange(a_other._mapping, nullptr);
			_view = std::exchange(a_other._view, nullptr);
			_viewSize = std::exchange(a_other._viewSize, 0);
			_version = a_other._version;
			_flags = a_other._flags;
			_entries = std::move(a_other._entries);
		}
		return *this;
	}

	BsaArchive::~BsaArchive()
	{
		Close();
	}

	void BsaArchive::Close() noexcept
	{
		if (_view) {
			UnmapViewOfFile(_view);
			_view = nullptr;
		}
		if (_mapping) {
			CloseHandle(_mapping);
			_mapping = nullptr;
		}
		if (_file) {
			CloseHandle(_file);
			_file = nullptr;
		}
	}

	std::vector<std::uint8_t> BsaArchive::Read(const Entry& a_entry) const
	{
		auto size = a_entry.size & ~kSizeCompressionToggle;
		const bool toggled = (a_entry.size & kSizeCompressionToggle) != 0;
		const bool compressed = toggled != ((_flags & kFlagCompressedByDefault) != 0);
		ViewReader reader(_view, _viewSize, a_entry.offset);
		if ((_flags & kFlagEmbeddedNames) != 0 && _version != 103u) {
			const auto nameLength = reader.Read<std::uint8_t>();
			(void)reader.ReadBytes(nameLength);
			if (size < nameLength + 1u) {
				throw std::runtime_error("embedded-name archive payload is truncated");
			}
			size -= nameLength + 1u;
		}

		if (!compressed) {
			const auto bytes = reader.ReadBytes(size);
			return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
		}

		if (size < sizeof(std::uint32_t)) {
			throw std::runtime_error("compressed archive payload is missing its size");
		}
		const auto originalSize = reader.Read<std::uint32_t>();
		const auto payload = reader.ReadBytes(size - sizeof(std::uint32_t));
		std::vector<std::uint8_t> output(originalSize);
		if (_version == 105u) {
			LZ4F_decompressionContext_t context = nullptr;
			if (LZ4F_isError(LZ4F_createDecompressionContext(&context, LZ4F_VERSION))) {
				throw std::runtime_error("LZ4 context creation failed");
			}
			std::size_t outSize = output.size();
			std::size_t inSize = payload.size();
			const auto result = LZ4F_decompress(context, output.data(), &outSize, payload.data(), &inSize, nullptr);
			LZ4F_freeDecompressionContext(context);
			if (LZ4F_isError(result) || outSize != output.size()) {
				throw std::runtime_error("LZ4 decompression failed");
			}
		} else {
			uLongf outSize = static_cast<uLongf>(output.size());
			const auto result = uncompress(output.data(), &outSize, reinterpret_cast<const Bytef*>(payload.data()), static_cast<uLong>(payload.size()));
			if (result != Z_OK || outSize != output.size()) {
				throw std::runtime_error("zlib decompression failed");
			}
		}
		return output;
	}

	ArchiveResolver::ArchiveResolver(std::filesystem::path a_dataPath, std::span<const std::string> a_archiveOrder) :
		_dataPath(std::move(a_dataPath))
	{
		std::unordered_set<std::string> seen;
		for (const auto& name : a_archiveOrder) {
			if (!seen.insert(LowerAscii(name)).second) {
				continue;
			}
			const auto path = _dataPath / name;
			std::error_code ec;
			if (!std::filesystem::is_regular_file(path, ec)) {
				continue;
			}
			try {
				auto archive = std::make_unique<BsaArchive>(path);
				const auto archiveIndex = static_cast<std::uint32_t>(_archives.size());
				for (const auto& [identity, entry] : archive->Entries()) {
					_index.insert_or_assign(identity, Location{ .archive = archiveIndex, .entry = entry });
				}
				spdlog::info("archive {}: {} file(s)", name, archive->Entries().size());
				_archives.push_back(std::move(archive));
			} catch (const std::exception& e) {
				spdlog::warn("skipped archive {}: {}", path.string(), e.what());
			}
		}
	}

	std::optional<std::vector<std::uint8_t>> ArchiveResolver::Read(std::string_view a_path) const
	{
		const auto canonical = CanonicalizeResourcePath(a_path);
		if (canonical.empty()) {
			return std::nullopt;
		}

		const auto loosePath = _dataPath / std::filesystem::path(canonical);
		std::error_code ec;
		if (std::filesystem::is_regular_file(loosePath, ec)) {
			std::ifstream input(loosePath, std::ios::binary);
			std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::filesystem::file_size(loosePath, ec)));
			if (!ec && input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
				return bytes;
			}
		}

		const auto it = _index.find(canonical);
		if (it == _index.end()) {
			return std::nullopt;
		}
		return _archives[it->second.archive]->Read(it->second.entry);
	}

	std::vector<std::string> DefaultArchiveOrder(std::span<const GameData::LoadOrderEntry> a_loadOrder)
	{
		std::vector<std::string> order{
			"Skyrim - Misc.bsa",
			"Skyrim - Shaders.bsa",
			"Skyrim - Interface.bsa",
			"Skyrim - Animations.bsa",
			"Skyrim - Meshes0.bsa",
			"Skyrim - Meshes1.bsa",
			"Skyrim - Sounds.bsa",
			"Skyrim - Voices_en0.bsa",
			"Skyrim - Textures0.bsa",
			"Skyrim - Textures1.bsa",
			"Skyrim - Textures2.bsa",
			"Skyrim - Textures3.bsa",
			"Skyrim - Textures4.bsa",
			"Skyrim - Textures5.bsa",
			"Skyrim - Textures6.bsa",
			"Skyrim - Textures7.bsa",
			"Skyrim - Textures8.bsa",
			"Skyrim - Patch.bsa",
		};
		for (const auto& entry : a_loadOrder) {
			const auto stem = std::filesystem::path(entry.pluginName).stem().string();
			order.push_back(stem + ".bsa");
			order.push_back(stem + " - Textures.bsa");
		}
		return order;
	}
}
