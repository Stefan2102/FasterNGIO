#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>

namespace FasterNGIO::GameData
{
	// A record or subrecord signature as it is stored: four ASCII characters, little-endian.
	using FourCC = std::uint32_t;

	constexpr FourCC MakeFourCC(char a, char b, char c, char d)
	{
		return static_cast<FourCC>(static_cast<unsigned char>(a)) |
		       (static_cast<FourCC>(static_cast<unsigned char>(b)) << 8) |
		       (static_cast<FourCC>(static_cast<unsigned char>(c)) << 16) |
		       (static_cast<FourCC>(static_cast<unsigned char>(d)) << 24);
	}

	enum class ModuleKind
	{
		Full,
		Light
	};

	// A plugin's place in the load order: the high byte of its form IDs (full), or FE plus a
	// 12-bit slot (light).
	struct FileID
	{
		ModuleKind kind{ ModuleKind::Full };
		std::uint16_t slot{ 0 };

		[[nodiscard]] std::uint32_t BaseFormID() const
		{
			return kind == ModuleKind::Light ? 0xFE000000u | (static_cast<std::uint32_t>(slot & 0x0FFFu) << 12) : static_cast<std::uint32_t>(slot) << 24;
		}

		friend bool operator==(const FileID&, const FileID&) = default;
	};

	// A load-order form ID (plugin-local IDs are converted while parsing).
	struct FormID
	{
		std::uint32_t value{ 0 };

		[[nodiscard]] bool IsNull() const { return value == 0; }
		[[nodiscard]] bool IsNone() const { return value == 0xFFFFFFFFu; }
		// Null or none: refers to no record.
		[[nodiscard]] bool IsEmpty() const { return IsNull() || IsNone(); }
		[[nodiscard]] bool IsHardcoded() const { return value < 0x800u; }

		friend bool operator==(const FormID&, const FormID&) = default;
		friend auto operator<=>(const FormID&, const FormID&) = default;
	};

	struct FormIDHash
	{
		std::size_t operator()(FormID a_id) const noexcept { return a_id.value; }
	};
}
