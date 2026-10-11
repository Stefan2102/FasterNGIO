#include "CommandLine.h"
#include "Generate.h"
#include "Gui/Launcher.h"

#include <oneapi/tbb/global_control.h>
#include <spdlog/spdlog.h>

#ifdef _WIN32
#include <Windows.h>
#endif

#include <algorithm>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	// The command line, run or handed to the launcher window.
	int RunMain(const std::vector<std::string_view>& a_args)
	{
		using namespace FasterNGIO;

		// No arguments (a double-click) or --gui: the launcher window. Any other arguments, as Mod
		// Organizer 2 passes them, run the command line unchanged.
		if (a_args.empty() || (a_args.size() == 1 && a_args.front() == "--gui")) {
			return Gui::RunLauncher();
		}
		try {
			const auto options = App::ParseCommandLine(a_args);
			if (!options) {
				App::PrintUsage();
				return 0;
			}
			std::optional<oneapi::tbb::global_control> threadLimit;
			if (options->threads > 0) {
				// The main thread only waits while the pipeline runs, so it does not count as a worker: a limit
				// of n alone would leave --threads 1 with no thread to run anything.
				threadLimit.emplace(oneapi::tbb::global_control::max_allowed_parallelism, options->threads + 1);
			}
			return App::Run(*options).exitCode;
		} catch (const std::exception& e) {
			spdlog::error("{}", e.what());
			return 1;
		}
	}

#ifdef _WIN32
	// Narrow argv would come in through the ANSI code page, which cannot represent every path (mod
	// folders are not ASCII) and turns the rest into '?'. wmain's UTF-16 does, so it is the entry
	// point and its arguments become UTF-8 text here, the program's path encoding.
	[[nodiscard]] std::string Utf8(const wchar_t* const a_text)
	{
		const auto bytes = WideCharToMultiByte(CP_UTF8, 0, a_text, -1, nullptr, 0, nullptr, nullptr);
		if (bytes <= 1) {
			return {};
		}
		std::string result(static_cast<std::size_t>(bytes) - 1, '\0');
		WideCharToMultiByte(CP_UTF8, 0, a_text, -1, result.data(), bytes, nullptr, nullptr);
		return result;
	}

	int wmainToUtf8(int a_argc, wchar_t** a_argv)
	{
		std::vector<std::string> storage;
		storage.reserve(static_cast<std::size_t>(a_argc));
		for (int i = 1; i < a_argc; ++i) {
			storage.push_back(Utf8(a_argv[i]));
		}
		const std::vector<std::string_view> args(storage.begin(), storage.end());
		return RunMain(args);
	}
#endif
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
	return wmainToUtf8(argc, argv);
}
#else
int main(int argc, char** argv)
{
	const std::vector<std::string_view> args(argv + 1, argv + argc);
	return RunMain(args);
}
#endif
