#pragma once

#include "GameData/GameData.h"
#include "Platform/DataDirectory.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace FasterNGIO::Archives
{
	// Lowercase, backslash-separated, no leading "data\" or separators: the key every lookup uses.
	[[nodiscard]] std::string CanonicalizeResourcePath(std::string_view a_path);

	// A read-only memory-mapped BSA (versions 103, 104 and 105). Entry reads are thread-safe.
	class BsaArchive
	{
	public:
		struct Entry
		{
			std::uint32_t size{ 0 };
			std::uint32_t offset{ 0 };
		};

		BsaArchive() = default;
		BsaArchive(const BsaArchive&) = delete;
		BsaArchive& operator=(const BsaArchive&) = delete;
		BsaArchive(BsaArchive&&) noexcept;
		BsaArchive& operator=(BsaArchive&&) noexcept;
		~BsaArchive();

		// Throws std::runtime_error when the file is not a supported archive.
		explicit BsaArchive(const std::filesystem::path& a_path);

		[[nodiscard]] std::vector<std::uint8_t> Read(const Entry& a_entry) const;
		[[nodiscard]] const std::filesystem::path& Path() const { return _path; }
		[[nodiscard]] const std::vector<std::pair<std::string, Entry>>& Entries() const { return _entries; }

	private:
		void Close() noexcept;

		std::filesystem::path _path;
		void* _file{ nullptr };
		void* _mapping{ nullptr };
		int _descriptor{ -1 };
		const std::uint8_t* _view{ nullptr };
		std::uint64_t _viewSize{ 0 };
		std::uint32_t _version{ 0 };
		std::uint32_t _flags{ 0 };
		std::vector<std::pair<std::string, Entry>> _entries;
	};

	// Resolves game resource paths the way the engine does: loose files in Data win, then the
	// last archive (in archive load order) that contains the path.
	class ArchiveResolver
	{
	public:
		// a_archiveOrder lists archive file names (not paths) in increasing precedence.
		ArchiveResolver(std::filesystem::path a_dataPath, std::span<const std::string> a_archiveOrder);

		[[nodiscard]] std::optional<std::vector<std::uint8_t>> Read(std::string_view a_path) const;
		[[nodiscard]] std::size_t ArchiveCount() const { return _archives.size(); }

	private:
		struct Location
		{
			std::uint32_t archive{ 0 };
			BsaArchive::Entry entry;
		};

		Platform::DataDirectory _data;
		std::vector<std::unique_ptr<BsaArchive>> _archives;
		std::unordered_map<std::string, Location> _index;
	};

	// The engine's archive order: sResourceArchiveList/sResourceArchiveList2 defaults, then
	// "<plugin>.bsa" and "<plugin> - Textures.bsa" for each plugin in load order.
	[[nodiscard]] std::vector<std::string> DefaultArchiveOrder(std::span<const GameData::LoadOrderEntry> a_loadOrder);
}
