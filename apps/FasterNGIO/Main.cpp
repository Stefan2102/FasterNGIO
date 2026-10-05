#include "CommandLine.h"
#include "Generate.h"
#include "Gui/Launcher.h"

#include <oneapi/tbb/global_control.h>
#include <spdlog/spdlog.h>

#include <exception>
#include <optional>
#include <string_view>
#include <vector>

int main(int argc, char** argv)
{
	using namespace FasterNGIO;

	// No arguments (a double-click) or --gui: the launcher window. Any other arguments, as Mod
	// Organizer 2 passes them, run the command line unchanged.
	if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--gui")) {
		return Gui::RunLauncher();
	}
	try {
		const std::vector<std::string_view> args(argv + 1, argv + argc);
		const auto options = App::ParseCommandLine(args);
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
