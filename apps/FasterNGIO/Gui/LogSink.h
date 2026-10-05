#pragma once

#include "Pipeline/MpscQueue.h"

#include <spdlog/sinks/sink.h>

#include <string>

namespace FasterNGIO::Gui
{
	struct LogLine
	{
		spdlog::level::level_enum level{ spdlog::level::info };
		std::string text;
	};

	// Forwards log messages from any thread to the launcher's log view through a lock-free queue;
	// the UI thread drains it each frame.
	class QueueSink final : public spdlog::sinks::sink
	{
	public:
		void log(const spdlog::details::log_msg& a_message) override
		{
			if (!should_log(a_message.level)) {
				return;
			}
			_queue.Push(LogLine{ a_message.level, std::string(a_message.payload.data(), a_message.payload.size()) });
		}

		void flush() override {}
		void set_pattern(const std::string&) override {}
		void set_formatter(std::unique_ptr<spdlog::formatter>) override {}

		// UI thread only.
		[[nodiscard]] std::optional<LogLine> TryPop() { return _queue.TryPop(); }

	private:
		Pipeline::MpscQueue<LogLine> _queue;
	};
}
