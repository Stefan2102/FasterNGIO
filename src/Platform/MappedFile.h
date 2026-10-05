#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace FasterNGIO::Platform
{
	// A whole file mapped read-only into memory (read into memory when it cannot be mapped). Bytes()
	// is safe to read from any thread for the object's lifetime.
	class MappedFile
	{
	public:
		// How the file will be read, a hint to the OS's read-ahead.
		enum class Access
		{
			Sequential,
			Random
		};

		// Throws std::runtime_error when the file cannot be opened or read.
		MappedFile(const std::filesystem::path& a_path, Access a_access);
		MappedFile(const MappedFile&) = delete;
		MappedFile& operator=(const MappedFile&) = delete;
		MappedFile(MappedFile&& a_other) noexcept;
		MappedFile& operator=(MappedFile&& a_other) noexcept;
		~MappedFile();

		[[nodiscard]] std::span<const std::uint8_t> Bytes() const noexcept { return _bytes; }

	private:
		void Close() noexcept;

		std::span<const std::uint8_t> _bytes;
		// Set when the file was read instead of mapped.
		std::vector<std::uint8_t> _owned;
		void* _view{ nullptr };
#if defined(_WIN32)
		void* _file{ nullptr };
		void* _mapping{ nullptr };
#endif
	};
}
