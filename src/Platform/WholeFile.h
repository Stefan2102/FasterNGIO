#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace FasterNGIO::Platform
{
	// Creates (or replaces) a_path with a_bytes, with as few system calls as the OS allows, so that it
	// never exists truncated: on Windows the file is created delete-on-close and kept only once every
	// byte is written (one WriteFile); elsewhere it is written to "<a_path>.tmp" and renamed over a_path. If the write fails
	// or the process dies, a_path is gone (Windows) or unchanged (elsewhere). Throws
	// std::runtime_error on failure.
	void WriteWholeFile(const std::filesystem::path& a_path, std::span<const std::uint8_t> a_bytes);

	// a_path's contents, or nullopt when it cannot be opened or read.
	[[nodiscard]] std::optional<std::vector<std::uint8_t>> ReadWholeFile(const std::filesystem::path& a_path);
}
