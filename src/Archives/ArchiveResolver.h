#pragma once

#include "GameData/LoadOrder.h"
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

	class BsaArchive;

	// Resolves game resource paths the way the engine does: loose files in Data win, then the
	// last archive (in archive load order) that contains the path. Reads are thread-safe.
	class ArchiveResolver
	{
	public:
		// a_archiveOrder lists archive file names (not paths) in increasing precedence.
		ArchiveResolver(std::filesystem::path a_dataPath, std::span<const std::string> a_archiveOrder);
		ArchiveResolver(ArchiveResolver&&) noexcept;
		ArchiveResolver& operator=(ArchiveResolver&&) noexcept;
		~ArchiveResolver();

		[[nodiscard]] std::optional<std::vector<std::uint8_t>> Read(std::string_view a_path) const;

	private:
		struct Location
		{
			std::uint32_t archive{ 0 };
			std::uint32_t entry{ 0 };
		};

		Platform::DataDirectory _data;
		std::vector<std::unique_ptr<BsaArchive>> _archives;
		std::unordered_map<std::string, Location> _index;
	};

	// [Archive] sResourceArchiveList and sResourceArchiveList2 as the game's INIs set them (archive
	// file names); a list the INIs leave out keeps the game's default.
	struct ArchiveIniLists
	{
		std::optional<std::vector<std::string>> resourceArchiveList;
		std::optional<std::vector<std::string>> resourceArchiveList2;
	};

	// sResourceArchiveList and sResourceArchiveList2 (the vanilla archives unless a_ini changes them),
	// then "<plugin>.bsa" and "<plugin> - Textures.bsa" for each plugin in load order.
	[[nodiscard]] std::vector<std::string> DefaultArchiveOrder(std::span<const GameData::LoadOrderEntry> a_loadOrder, const ArchiveIniLists& a_ini = {});
}
