#pragma once

#include <ORGModuleServices/Async/GraphScheduler.h>

#include <memory>

namespace FasterNGIO::Pipeline
{
	// org::async::GraphScheduler on oneTBB: every task (controlled or CPU) is spawned into one
	// task arena that owns all hardware threads, so graph producers scale with the core count.
	// Delayed tasks are rare (graph retries) and run from a short-lived timer thread.
	[[nodiscard]] std::shared_ptr<org::async::GraphScheduler> MakeTbbGraphScheduler();
}
