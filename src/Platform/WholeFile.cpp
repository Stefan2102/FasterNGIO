#include "Platform/WholeFile.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace FasterNGIO::Platform
{
#if defined(_WIN32)
	namespace
	{
		// Whether files can be created delete-on-close and then kept (FileDispositionInfoEx with
		// FILE_DISPOSITION_FLAG_ON_CLOSE: NTFS since Windows 10 1709). Cleared at the first refusal.
		std::atomic<bool> g_deleteOnCloseAtCreate{ true };

		// Writes a_path so that it is deleted if the handle closes before every byte is written (a
		// failed write, or the process killed or crashing): with a_atCreate, from the create call on;
		// otherwise from just after it, which leaves an empty file if the process dies inside the
		// create (where antivirus scanning spends most of a file's time). A temporary file renamed
		// into place would do the same at twice the cost per file: NTFS and the antivirus filter
		// charge a rename about as much as a create. False when the filesystem refused to keep a file
		// created delete-on-close (it is gone; the caller retries without a_atCreate).
		bool WriteDeleteOnFailure(const std::filesystem::path& a_path, std::span<const std::uint8_t> a_bytes, bool a_atCreate)
		{
			const DWORD flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | (a_atCreate ? FILE_FLAG_DELETE_ON_CLOSE : 0);
			const HANDLE file = CreateFileW(a_path.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_ALWAYS, flags, nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				throw std::runtime_error("cannot create " + a_path.string() + " (error " + std::to_string(GetLastError()) + ")");
			}
			const auto setDeleteOnClose = [&](bool a_delete) {
				FILE_DISPOSITION_INFO disposition{ .DeleteFile = static_cast<BOOLEAN>(a_delete) };
				return SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition)) != 0;
			};
			bool ok = a_atCreate || setDeleteOnClose(true);
			std::size_t offset = 0;
			while (ok && offset < a_bytes.size()) {
				const auto chunk = static_cast<DWORD>((std::min<std::size_t>)(a_bytes.size() - offset, 1u << 30));
				DWORD written = 0;
				ok = WriteFile(file, a_bytes.data() + offset, chunk, &written, nullptr) != 0 && written != 0;
				offset += written;
			}
			bool kept = true;
			if (ok && a_atCreate) {
				// ON_CLOSE without DELETE clears the delete-on-close state the create set.
				FILE_DISPOSITION_INFO_EX keep{ .Flags = FILE_DISPOSITION_FLAG_ON_CLOSE };
				kept = SetFileInformationByHandle(file, FileDispositionInfoEx, &keep, sizeof(keep)) != 0;
			} else if (ok) {
				ok = setDeleteOnClose(false);
			}
			const auto error = ok ? 0 : GetLastError();
			if (!ok && !a_atCreate) {
				setDeleteOnClose(true);
			}
			CloseHandle(file);
			if (!ok) {
				throw std::runtime_error("cannot write " + a_path.string() + " (error " + std::to_string(error) + ")");
			}
			return kept;
		}
	}
#endif

	void WriteWholeFile(const std::filesystem::path& a_path, std::span<const std::uint8_t> a_bytes)
	{
		// An interrupted run or a failed write must never leave a truncated file under the real name:
		// the game would read it as a cache.
#if defined(_WIN32)
		if (g_deleteOnCloseAtCreate.load(std::memory_order_relaxed)) {
			if (WriteDeleteOnFailure(a_path, a_bytes, true)) {
				return;
			}
			g_deleteOnCloseAtCreate.store(false, std::memory_order_relaxed);
		}
		WriteDeleteOnFailure(a_path, a_bytes, false);
#else
		// Written beside the target and renamed over it (a rename is cheap here).
		auto temporary = a_path;
		temporary += ".tmp";
		const int file = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (file < 0) {
			throw std::runtime_error("cannot create " + temporary.string() + ": " + std::strerror(errno));
		}
		const auto fail = [&](std::string_view a_what, int a_error) {
			::unlink(temporary.c_str());
			throw std::runtime_error(std::string(a_what) + " " + a_path.string() + ": " + std::strerror(a_error));
		};
		std::size_t offset = 0;
		while (offset < a_bytes.size()) {
			const auto written = ::write(file, a_bytes.data() + offset, a_bytes.size() - offset);
			if (written < 0 && errno == EINTR) {
				continue;
			}
			if (written <= 0) {
				const int error = errno;
				::close(file);
				fail("cannot write", error);
			}
			offset += static_cast<std::size_t>(written);
		}
		if (::close(file) != 0) {
			fail("cannot write", errno);
		}
		if (::rename(temporary.c_str(), a_path.c_str()) != 0) {
			fail("cannot replace", errno);
		}
#endif
	}

	std::optional<std::vector<std::uint8_t>> ReadWholeFile(const std::filesystem::path& a_path)
	{
		std::ifstream file(a_path, std::ios::binary | std::ios::ate);
		if (!file) {
			return std::nullopt;
		}
		const auto size = file.tellg();
		if (size < 0) {
			return std::nullopt;
		}
		std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
		file.seekg(0);
		if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
			return std::nullopt;
		}
		return bytes;
	}
}
