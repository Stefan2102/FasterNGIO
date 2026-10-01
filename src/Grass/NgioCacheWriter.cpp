#include "Grass/NgioCacheWriter.h"

#include <fstream>
#include <stdexcept>

namespace FasterNGIO::Grass
{
	namespace
	{
		void WriteU32(std::ostream& a_output, std::uint32_t a_value)
		{
			const std::array<char, 4> bytes{
				static_cast<char>(a_value & 0xFFu),
				static_cast<char>((a_value >> 8) & 0xFFu),
				static_cast<char>((a_value >> 16) & 0xFFu),
				static_cast<char>((a_value >> 24) & 0xFFu),
			};
			a_output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		}

		void WriteByte(std::ostream& a_output, bool a_value)
		{
			const char byte = a_value ? 1 : 0;
			a_output.write(std::addressof(byte), 1);
		}
	}

	void WriteNgioCellCache(const std::filesystem::path& a_path, const NgioCellCache& a_cache)
	{
		std::filesystem::create_directories(a_path.parent_path());

		std::ofstream output(a_path, std::ios::binary | std::ios::trunc);
		if (!output) {
			throw std::runtime_error("failed to open NGIO cache file for writing: " + a_path.string());
		}

		WriteU32(output, static_cast<std::uint32_t>(a_cache.groups.size()));
		for (const auto& group : a_cache.groups) {
			WriteU32(output, static_cast<std::uint32_t>(group.modelPath.size() + 1));
			output.write(group.modelPath.data(), static_cast<std::streamsize>(group.modelPath.size()));
			output.put('\0');

			WriteU32(output, group.grassModelData);
			WriteU32(output, group.grassFormID);
			WriteByte(output, group.vertexLighting);
			WriteByte(output, group.uniformScaling);
			WriteByte(output, group.fitToSlope);
			WriteU32(output, static_cast<std::uint32_t>(group.blocks.size()));

			for (const auto& block : group.blocks) {
				for (const auto word : block.descriptorWords) {
					WriteU32(output, word);
				}
				for (const auto word : block.payloadWords) {
					const std::array<char, 2> bytes{
						static_cast<char>(word & 0xFFu),
						static_cast<char>((word >> 8) & 0xFFu),
					};
					output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
				}
			}
		}

		if (!output) {
			throw std::runtime_error("failed while writing NGIO cache file: " + a_path.string());
		}
	}
}
