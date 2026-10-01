#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace FasterNGIO::GameData::Internal
{
	[[nodiscard]] std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& a_path);

	// Worker-safe after construction. The creating scope exclusively owns mapping lifetime.
	class FileByteView
	{
	public:
		FileByteView() = default;
		FileByteView(const FileByteView&) = delete;
		FileByteView& operator=(const FileByteView&) = delete;
		FileByteView(FileByteView&& a_rhs) noexcept;
		FileByteView& operator=(FileByteView&& a_rhs) noexcept;
		~FileByteView();
		[[nodiscard]] std::span<const std::uint8_t> Span() const noexcept;
	private:
		void Close() noexcept;
		std::vector<std::uint8_t> _ownedBytes;
		std::span<const std::uint8_t> _bytes;
#ifdef _WIN32
		void* _mappedView{ nullptr };
		HANDLE _mappingHandle{ nullptr };
		HANDLE _fileHandle{ INVALID_HANDLE_VALUE };
#endif
		friend FileByteView MapFileBytes(const std::filesystem::path& a_path);
	};

	[[nodiscard]] FileByteView MapFileBytes(const std::filesystem::path& a_path);
}
