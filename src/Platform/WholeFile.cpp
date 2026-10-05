#include "Platform/WholeFile.h"

#include <algorithm>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace FasterNGIO::Platform
{
	void WriteWholeFile(const std::filesystem::path& a_path, std::span<const std::uint8_t> a_bytes)
	{
#if defined(_WIN32)
		const HANDLE file = CreateFileW(a_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (file == INVALID_HANDLE_VALUE) {
			throw std::runtime_error("cannot create " + a_path.string() + " (error " + std::to_string(GetLastError()) + ")");
		}
		std::size_t offset = 0;
		bool ok = true;
		while (ok && offset < a_bytes.size()) {
			const auto chunk = static_cast<DWORD>((std::min<std::size_t>)(a_bytes.size() - offset, 1u << 30));
			DWORD written = 0;
			ok = WriteFile(file, a_bytes.data() + offset, chunk, &written, nullptr) != 0 && written != 0;
			offset += written;
		}
		const auto error = ok ? 0 : GetLastError();
		CloseHandle(file);
		if (!ok) {
			throw std::runtime_error("cannot write " + a_path.string() + " (error " + std::to_string(error) + ")");
		}
#else
		const int file = ::open(a_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (file < 0) {
			throw std::runtime_error("cannot create " + a_path.string() + ": " + std::strerror(errno));
		}
		std::size_t offset = 0;
		while (offset < a_bytes.size()) {
			const auto written = ::write(file, a_bytes.data() + offset, a_bytes.size() - offset);
			if (written < 0 && errno == EINTR) {
				continue;
			}
			if (written <= 0) {
				const int error = errno;
				::close(file);
				throw std::runtime_error("cannot write " + a_path.string() + ": " + std::strerror(error));
			}
			offset += static_cast<std::size_t>(written);
		}
		if (::close(file) != 0) {
			throw std::runtime_error("cannot write " + a_path.string() + ": " + std::strerror(errno));
		}
#endif
	}
}
