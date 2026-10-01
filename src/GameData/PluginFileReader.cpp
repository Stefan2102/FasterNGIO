#include "GameData/Internal/PluginFileReader.h"

#include <fstream>
#include <memory>
#include <stdexcept>
#include <utility>

namespace FasterNGIO::GameData::Internal
{
	std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& a_path)
		{
			std::ifstream input(a_path, std::ios::binary);
			if (!input) throw std::runtime_error("failed to open plugin file: " + a_path.string());
			input.seekg(0, std::ios::end);
			const auto size = input.tellg();
			if (size < 0) throw std::runtime_error("failed to size plugin file: " + a_path.string());
			input.seekg(0, std::ios::beg);
			std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
			if (!bytes.empty()) {
				input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
				if (!input) throw std::runtime_error("failed to read plugin file: " + a_path.string());
			}
			return bytes;
		}

	FileByteView::FileByteView(FileByteView&& a_rhs) noexcept { *this = std::move(a_rhs); }
	FileByteView& FileByteView::operator=(FileByteView&& a_rhs) noexcept
	{
		if (this != std::addressof(a_rhs)) {
			Close();
			_ownedBytes = std::move(a_rhs._ownedBytes);
			_bytes = _ownedBytes.empty() ? a_rhs._bytes : std::span<const std::uint8_t>{ _ownedBytes };
#ifdef _WIN32
			_mappedView = a_rhs._mappedView; _mappingHandle = a_rhs._mappingHandle; _fileHandle = a_rhs._fileHandle;
			a_rhs._mappedView = nullptr; a_rhs._mappingHandle = nullptr; a_rhs._fileHandle = INVALID_HANDLE_VALUE;
#endif
			a_rhs._bytes = {};
		}
		return *this;
	}
	FileByteView::~FileByteView() { Close(); }
	std::span<const std::uint8_t> FileByteView::Span() const noexcept { return _bytes; }
	void FileByteView::Close() noexcept
	{
#ifdef _WIN32
		if (_mappedView) { UnmapViewOfFile(_mappedView); _mappedView = nullptr; }
		if (_mappingHandle) { CloseHandle(_mappingHandle); _mappingHandle = nullptr; }
		if (_fileHandle != INVALID_HANDLE_VALUE) { CloseHandle(_fileHandle); _fileHandle = INVALID_HANDLE_VALUE; }
#endif
		_bytes = {};
	}

	FileByteView MapFileBytes(const std::filesystem::path& a_path)
	{
		FileByteView result;
#ifdef _WIN32
		result._fileHandle = CreateFileW(a_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (result._fileHandle != INVALID_HANDLE_VALUE) {
			LARGE_INTEGER fileSize{};
			if (GetFileSizeEx(result._fileHandle, std::addressof(fileSize)) && fileSize.QuadPart >= 0) {
				result._mappingHandle = CreateFileMappingW(result._fileHandle, nullptr, PAGE_READONLY, 0, 0, nullptr);
				if (result._mappingHandle) {
					result._mappedView = MapViewOfFile(result._mappingHandle, FILE_MAP_READ, 0, 0, 0);
					if (result._mappedView) {
						result._bytes = { static_cast<const std::uint8_t*>(result._mappedView), static_cast<std::size_t>(fileSize.QuadPart) };
						return result;
					}
				}
			}
			result.Close();
		}
#endif
		result._ownedBytes = ReadFileBytes(a_path);
		result._bytes = result._ownedBytes;
		return result;
	}
}
