#include "GameData/GameData.h"

#include <algorithm>
#include <cctype>
#include <ranges>
#include <unordered_set>

namespace FasterNGIO::GameData
{
	namespace
	{
		std::string LowerAsciiPath(std::string_view a_value)
		{
			std::string result(a_value);
			std::ranges::transform(result, result.begin(), [](unsigned char ch) {
				return static_cast<char>(std::tolower(ch));
			});
			return result;
		}

		std::string TrimPath(std::string_view a_value)
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

		constexpr FourCC SIG_MODL = MakeFourCC('M', 'O', 'D', 'L');
		constexpr FourCC SIG_MOD2 = MakeFourCC('M', 'O', 'D', '2');
		constexpr FourCC SIG_MOD3 = MakeFourCC('M', 'O', 'D', '3');
		constexpr FourCC SIG_MOD4 = MakeFourCC('M', 'O', 'D', '4');
		constexpr FourCC SIG_MOD5 = MakeFourCC('M', 'O', 'D', '5');
		constexpr FourCC SIG_MNAM = MakeFourCC('M', 'N', 'A', 'M');
		constexpr FourCC SIG_STAT = MakeFourCC('S', 'T', 'A', 'T');

		bool IsModelPathSubrecord(FourCC a_signature)
		{
			return a_signature == SIG_MODL || a_signature == SIG_MOD2 || a_signature == SIG_MOD3 ||
			       a_signature == SIG_MOD4 || a_signature == SIG_MOD5;
		}

		std::span<const std::uint8_t> SubrecordData(const RawRecord& a_record, const RawSubrecord& a_subrecord)
		{
			const auto data = a_record.Data();
			if (a_subrecord.dataOffset > data.size() || a_subrecord.size > data.size() - a_subrecord.dataOffset) return {};
			return data.subspan(a_subrecord.dataOffset, a_subrecord.size);
		}

		std::string ReadString(std::span<const std::uint8_t> a_data)
		{
			const auto* begin = reinterpret_cast<const char*>(a_data.data());
			const auto* end = begin + a_data.size();
			return std::string(begin, std::find(begin, end, '\0'));
		}

		std::string ReadString260(std::span<const std::uint8_t> a_data, std::size_t a_index)
		{
			const auto offset = a_index * 260u;
			if (offset >= a_data.size()) return {};
			return ReadString(a_data.subspan(offset, (std::min<std::size_t>)(260u, a_data.size() - offset)));
		}
	}

	std::string NormalizeModelPath(std::string_view a_path)
	{
		auto trimmed = TrimPath(a_path);
		while (!trimmed.empty() && trimmed.back() == '\0') {
			trimmed.pop_back();
		}
		trimmed = TrimPath(trimmed);
		if (trimmed.empty()) {
			return {};
		}

		std::replace(trimmed.begin(), trimmed.end(), '/', '\\');
		while (!trimmed.empty() && (trimmed.front() == '\\' || trimmed.front() == '/')) {
			trimmed.erase(trimmed.begin());
		}
		trimmed = LowerAsciiPath(trimmed);
		if (!trimmed.ends_with(".nif")) {
			return {};
		}
		if (!trimmed.starts_with("meshes\\")) {
			trimmed = "meshes\\" + trimmed;
		}
		return trimmed;
	}

	ModelPathIndex BuildModelPathIndex(const std::vector<PluginFile>& a_plugins, const ResolvedRecordStore& a_resolvedRecords)
	{
		ModelPathIndex index;
		index.references.reserve(a_resolvedRecords.chains.size() / 4);
		std::unordered_set<std::string> unique;
		unique.reserve(a_resolvedRecords.chains.size() / 8);
		const auto addPath = [&](const RawRecord& record, const RawSubrecord& subrecord, std::string originalPath, bool lodModel) {
			const auto normalized = NormalizeModelPath(originalPath);
			if (normalized.empty()) return;
			const auto sourcePlugin = record.sourceFileIndex < a_plugins.size() ? a_plugins[record.sourceFileIndex].entry.pluginName : std::string{};
			index.references.push_back({ normalized, std::move(originalPath), record.loadOrderFormID, record.signature,
				subrecord.signature, sourcePlugin, record.sourceFileIndex, lodModel });
			if (unique.insert(normalized).second) index.uniqueNormalizedPaths.push_back(normalized);
		};
		for (const auto& [formID, chain] : a_resolvedRecords.chains) {
			(void)formID;
			const auto* record = chain.Winning();
			if (!record || record->IsDeleted() || record->IsIgnored()) continue;
			for (const auto& subrecord : record->subrecords) {
				if (IsModelPathSubrecord(subrecord.signature)) {
					addPath(*record, subrecord, ReadString(SubrecordData(*record, subrecord)), false);
				} else if (subrecord.signature == SIG_MNAM && record->signature == SIG_STAT) {
					const auto lodData = SubrecordData(*record, subrecord);
					for (std::size_t i = 0; i < 4; ++i) addPath(*record, subrecord, ReadString260(lodData, i), true);
				}
			}
		}
		std::sort(index.uniqueNormalizedPaths.begin(), index.uniqueNormalizedPaths.end());
		return index;
	}
}
