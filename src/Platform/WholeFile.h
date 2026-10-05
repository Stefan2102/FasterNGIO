#pragma once

#include <cstdint>
#include <filesystem>
#include <span>

namespace FasterNGIO::Platform
{
	// Creates (or truncates) a_path and writes a_bytes with as few system calls as the OS allows:
	// one WriteFile on Windows, a write loop elsewhere. Throws std::runtime_error on failure.
	void WriteWholeFile(const std::filesystem::path& a_path, std::span<const std::uint8_t> a_bytes);
}
