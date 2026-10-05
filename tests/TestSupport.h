#pragma once

#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace FasterNGIO::Tests
{
	// A fresh folder under the system temp folder, removed with everything in it on destruction.
	// Names are random so parallel test processes never share one.
	class TempDirectory
	{
	public:
		TempDirectory()
		{
			std::random_device random;
			const auto suffix = std::to_string(random()) + "-" + std::to_string(random());
			_path = std::filesystem::temp_directory_path() / ("fasterngio-test-" + suffix);
			std::filesystem::create_directories(_path);
		}
		~TempDirectory()
		{
			std::error_code error;
			std::filesystem::remove_all(_path, error);
		}
		TempDirectory(const TempDirectory&) = delete;
		TempDirectory& operator=(const TempDirectory&) = delete;

		[[nodiscard]] const std::filesystem::path& Path() const { return _path; }

	private:
		std::filesystem::path _path;
	};

	// Writes a_text to a_path, creating its folders.
	inline void WriteText(const std::filesystem::path& a_path, std::string_view a_text = "x")
	{
		std::filesystem::create_directories(a_path.parent_path());
		std::ofstream(a_path, std::ios::binary) << a_text;
	}

	[[nodiscard]] inline std::vector<std::uint8_t> ReadAll(const std::filesystem::path& a_path)
	{
		std::ifstream file(a_path, std::ios::binary);
		return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
	}

	// The files InspectGameFolder needs to accept a_root as a Skyrim Special Edition install.
	inline void MakeGameFolder(const std::filesystem::path& a_root)
	{
		WriteText(a_root / "SkyrimSE.exe");
		WriteText(a_root / "Data" / "Skyrim.esm");
	}
}
