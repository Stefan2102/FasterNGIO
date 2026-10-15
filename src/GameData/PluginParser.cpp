#include "GameData/StaticWorld.h"

#include <algorithm>
#include <stdexcept>

namespace FasterNGIO::GameData
{
	StaticPluginShard ParseStaticWorldShard(const LoadOrderEntry& a_entry, std::span<const LoadOrderEntry> a_loadOrder)
	{
		const auto it = std::ranges::find(a_loadOrder, a_entry.path, &LoadOrderEntry::path);
		if (it == a_loadOrder.end())
			throw std::invalid_argument("plugin is not in the prepared load order");
		return tesworld::PluginParser{}.ParseStaticWorldShard(a_entry, static_cast<std::size_t>(it - a_loadOrder.begin()), a_loadOrder,
															  tesworld::PluginParser::ParseMode::StaticGrassIndex);
	}
} // namespace FasterNGIO::GameData
