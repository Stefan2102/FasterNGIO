#include "Platform/MappedFile.h"

#include "Platform/WholeFile.h"

#include <stdexcept>
#include <utility>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace FasterNGIO::Platform
{
	MappedFile::MappedFile(const std::filesystem::path& a_path, Access a_access)
	{
#if defined(_WIN32)
		const DWORD hint = a_access == Access::Random ? FILE_FLAG_RANDOM_ACCESS : FILE_FLAG_SEQUENTIAL_SCAN;
		const HANDLE file = CreateFileW(a_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | hint, nullptr);
		if (file != INVALID_HANDLE_VALUE) {
			_file = file;
			LARGE_INTEGER size{};
			if (GetFileSizeEx(file, &size) && size.QuadPart > 0) {
				_mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
				_view = _mapping ? MapViewOfFile(_mapping, FILE_MAP_READ, 0, 0, 0) : nullptr;
				if (_view) {
					_bytes = { static_cast<const std::uint8_t*>(_view), static_cast<std::size_t>(size.QuadPart) };
					return;
				}
			}
			Close();
		}
#else
		const int descriptor = ::open(a_path.c_str(), O_RDONLY | O_CLOEXEC);
		if (descriptor >= 0) {
			struct stat status{};
			if (::fstat(descriptor, &status) == 0 && status.st_size > 0) {
				const auto size = static_cast<std::size_t>(status.st_size);
				void* view = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, descriptor, 0);
				if (view != MAP_FAILED) {
					::madvise(view, size, a_access == Access::Random ? MADV_RANDOM : MADV_SEQUENTIAL);
					_view = view;
					_bytes = { static_cast<const std::uint8_t*>(view), size };
				}
			}
			// The mapping keeps its own reference to the file.
			::close(descriptor);
			if (_view) {
				return;
			}
		}
#endif
		// Empty files cannot be mapped, and some filesystems cannot map at all: read it instead.
		auto bytes = ReadWholeFile(a_path);
		if (!bytes) {
			throw std::runtime_error("failed to read " + a_path.string());
		}
		_owned = std::move(*bytes);
		_bytes = _owned;
	}

	MappedFile::MappedFile(MappedFile&& a_other) noexcept
	{
		*this = std::move(a_other);
	}

	MappedFile& MappedFile::operator=(MappedFile&& a_other) noexcept
	{
		if (this != std::addressof(a_other)) {
			Close();
			const bool owned = !a_other._owned.empty();
			_owned = std::move(a_other._owned);
			_bytes = owned ? std::span<const std::uint8_t>(_owned) : a_other._bytes;
			_view = std::exchange(a_other._view, nullptr);
#if defined(_WIN32)
			_file = std::exchange(a_other._file, nullptr);
			_mapping = std::exchange(a_other._mapping, nullptr);
#endif
			a_other._bytes = {};
		}
		return *this;
	}

	MappedFile::~MappedFile()
	{
		Close();
	}

	void MappedFile::Close() noexcept
	{
#if defined(_WIN32)
		if (_view) {
			UnmapViewOfFile(_view);
		}
		if (_mapping) {
			CloseHandle(_mapping);
		}
		if (_file) {
			CloseHandle(_file);
		}
		_file = nullptr;
		_mapping = nullptr;
#else
		if (_view) {
			::munmap(_view, _bytes.size());
		}
#endif
		_view = nullptr;
		_bytes = {};
		_owned.clear();
	}
}
