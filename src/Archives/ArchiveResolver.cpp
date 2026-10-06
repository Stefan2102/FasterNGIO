#include "Archives/ArchiveResolver.h"

#include "Platform/MappedFile.h"
#include "Platform/Text.h"
#include "Platform/WholeFile.h"

#include <lz4frame.h>
#include <spdlog/spdlog.h>
#include <zlib.h>

#include <algorithm>
#include <cstring>
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
		constexpr std::size_t kHeaderSize = 36;

		class ViewReader
		{
		public:
			ViewReader(std::span<const std::uint8_t> a_bytes, std::uint64_t a_offset = 0) :
				_bytes(a_bytes), _offset(a_offset) {}

			template <class T>
			[[nodiscard]] T Read()
			{
				Require(sizeof(T));
				T value{};
				std::memcpy(std::addressof(value), _bytes.data() + _offset, sizeof(T));
				_offset += sizeof(T);
				return value;
			}

			[[nodiscard]] std::string_view ReadBytes(std::uint64_t a_count)
			{
				Require(a_count);
				const std::string_view bytes(reinterpret_cast<const char*>(_bytes.data() + _offset), static_cast<std::size_t>(a_count));
				_offset += a_count;
				return bytes;
			}

			[[nodiscard]] std::string_view ReadZString()
			{
				Require(0);
				const auto* begin = _bytes.data() + _offset;
				const auto* end = static_cast<const std::uint8_t*>(std::memchr(begin, 0, static_cast<std::size_t>(_bytes.size() - _offset)));
				if (!end) {
					throw std::runtime_error("unterminated name in BSA");
				}
				const std::string_view text(reinterpret_cast<const char*>(begin), static_cast<std::size_t>(end - begin));
				_offset += text.size() + 1;
				return text;
			}

		private:
			void Require(std::uint64_t a_count) const
			{
				if (_offset + a_count > _bytes.size()) {
					throw std::runtime_error("unexpected end of BSA");
				}
			}

			std::span<const std::uint8_t> _bytes;
			std::uint64_t _offset;
		};
	}

	// A memory-mapped BSA (versions 103, 104 and 105). Entry reads are thread-safe.
	class BsaArchive
	{
	public:
		struct Entry
		{
			std::uint32_t size{ 0 };
			std::uint32_t offset{ 0 };
		};

		// Throws std::runtime_error when the file is not a supported archive.
		explicit BsaArchive(const std::filesystem::path& a_path);

		[[nodiscard]] std::vector<std::uint8_t> Read(const Entry& a_entry) const;
		// Canonical resource path and entry, in archive order.
		[[nodiscard]] const std::vector<std::pair<std::string, Entry>>& Entries() const { return _entries; }

	private:
		Platform::MappedFile _file;
		std::uint32_t _version{ 0 };
		std::uint32_t _flags{ 0 };
		std::vector<std::pair<std::string, Entry>> _entries;
	};

	std::string CanonicalizeResourcePath(std::string_view a_path)
	{
		std::string result = Platform::LowerAscii(a_path);
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
		_file(a_path, Platform::MappedFile::Access::Random)
	{
		const auto bytes = _file.Bytes();
		if (bytes.size() < kHeaderSize) {
			throw std::runtime_error("file too small");
		}
		ViewReader header(bytes);
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

		// Folder records (hash, file count, offset; 105 widens the offset), then each folder's name
		// and file records, then every file name in the same order.
		std::vector<std::uint32_t> folderFileCounts(folderCount);
		ViewReader reader(bytes, foldersOffset);
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
			auto canonical = CanonicalizeResourcePath(folderName.empty() ? std::string(fileName) : folderName + "\\" + std::string(fileName));
			if (!canonical.empty()) {
				_entries.emplace_back(std::move(canonical), records[i]);
			}
		}
	}

	std::vector<std::uint8_t> BsaArchive::Read(const Entry& a_entry) const
	{
		auto size = a_entry.size & ~kSizeCompressionToggle;
		const bool toggled = (a_entry.size & kSizeCompressionToggle) != 0;
		const bool compressed = toggled != ((_flags & kFlagCompressedByDefault) != 0);
		ViewReader reader(_file.Bytes(), a_entry.offset);
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
		_data(std::move(a_dataPath))
	{
		std::unordered_set<std::string> seen;
		for (const auto& name : a_archiveOrder) {
			if (!seen.insert(Platform::LowerAscii(name)).second) {
				continue;
			}
			const auto path = _data.Find(name);
			if (!path) {
				continue;
			}
			try {
				auto archive = std::make_unique<BsaArchive>(*path);
				const auto archiveIndex = static_cast<std::uint32_t>(_archives.size());
				const auto& entries = archive->Entries();
				for (std::uint32_t i = 0; i < entries.size(); ++i) {
					_index.insert_or_assign(entries[i].first, Location{ .archive = archiveIndex, .entry = i });
				}
				spdlog::debug("archive {}: {} file(s)", name, entries.size());
				_archives.push_back(std::move(archive));
			} catch (const std::exception& e) {
				spdlog::warn("skipped archive {}: {}", path->string(), e.what());
			}
		}
		spdlog::info("archives: {} loaded, {} file(s)", _archives.size(), _index.size());
	}

	ArchiveResolver::ArchiveResolver(ArchiveResolver&&) noexcept = default;
	ArchiveResolver& ArchiveResolver::operator=(ArchiveResolver&&) noexcept = default;
	ArchiveResolver::~ArchiveResolver() = default;

	std::optional<std::vector<std::uint8_t>> ArchiveResolver::Read(std::string_view a_path) const
	{
		const auto canonical = CanonicalizeResourcePath(a_path);
		if (canonical.empty()) {
			return std::nullopt;
		}

		// Loose files win, matched as the game matches them (case-insensitively, either separator).
		if (const auto loosePath = _data.Find(canonical)) {
			// An unreadable loose file falls through to the archives.
			if (auto bytes = Platform::ReadWholeFile(*loosePath)) {
				return bytes;
			}
		}

		const auto it = _index.find(canonical);
		if (it == _index.end()) {
			return std::nullopt;
		}
		const auto& archive = *_archives[it->second.archive];
		return archive.Read(archive.Entries()[it->second.entry].second);
	}

	std::vector<std::string> DefaultArchiveOrder(std::span<const GameData::LoadOrderEntry> a_loadOrder, const ArchiveIniLists& a_ini)
	{
		std::vector<std::string> order = a_ini.resourceArchiveList.value_or(std::vector<std::string>{
			"Skyrim - Misc.bsa",
			"Skyrim - Shaders.bsa",
			"Skyrim - Interface.bsa",
			"Skyrim - Animations.bsa",
			"Skyrim - Meshes0.bsa",
			"Skyrim - Meshes1.bsa",
			"Skyrim - Sounds.bsa",
		});
		const auto list2 = a_ini.resourceArchiveList2.value_or(std::vector<std::string>{
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
		});
		order.insert(order.end(), list2.begin(), list2.end());
		for (const auto& entry : a_loadOrder) {
			const auto stem = std::filesystem::path(entry.pluginName).stem().string();
			order.push_back(stem + ".bsa");
			order.push_back(stem + " - Textures.bsa");
		}
		return order;
	}
}
