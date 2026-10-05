#pragma once

#include "Generate.h"

namespace FasterNGIO::App
{
	// Runs the diagnostic a_options selects (--dump-collision, --collision-survey, --export-blades or
	// --benchmark-rejection) on the loaded plugins instead of generating caches. Returns its exit code.
	[[nodiscard]] int RunDiagnostic(const GenerateOptions& a_options, const LoadedPlugins& a_plugins);
}
