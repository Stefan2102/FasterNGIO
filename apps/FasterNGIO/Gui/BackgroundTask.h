#pragma once

#include <atomic>
#include <exception>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

namespace FasterNGIO::Gui
{
	// Work on its own thread whose result the UI thread polls: the result is written before the
	// state is published, so it may be read once Finished() is true. Destruction requests a stop
	// and joins.
	template <class Result>
	class BackgroundTask
	{
	public:
		template <class Function>
		explicit BackgroundTask(Function a_function) :
			_thread([this, function = std::move(a_function)](std::stop_token a_stop) {
				try {
					_result.emplace(function(a_stop));
					_state.store(State::Done, std::memory_order_release);
				} catch (const std::exception& e) {
					_error = e.what();
					_state.store(State::Failed, std::memory_order_release);
				}
			})
		{
		}

		BackgroundTask(const BackgroundTask&) = delete;
		BackgroundTask& operator=(const BackgroundTask&) = delete;

		[[nodiscard]] bool Finished() const { return _state.load(std::memory_order_acquire) != State::Running; }
		[[nodiscard]] bool Failed() const { return _state.load(std::memory_order_acquire) == State::Failed; }
		// Only once Finished().
		[[nodiscard]] const std::optional<Result>& Value() const { return _result; }
		[[nodiscard]] const std::string& Error() const { return _error; }
		void RequestStop() { _thread.request_stop(); }

	private:
		enum class State
		{
			Running,
			Done,
			Failed
		};

		std::atomic<State> _state{ State::Running };
		std::optional<Result> _result;
		std::string _error;
		// Last: it starts running once everything above exists, and joins first on destruction.
		std::jthread _thread;
	};
}
