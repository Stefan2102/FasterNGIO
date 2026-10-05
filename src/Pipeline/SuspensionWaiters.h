#pragma once

#include <oneapi/tbb/concurrent_queue.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

namespace FasterNGIO::Pipeline
{
	// Producers that found a resource exhausted and suspended in an AsyncStateGraph, woken oldest
	// first as it frees up. Lock-free: nothing here blocks.
	class SuspensionWaiters
	{
	public:
		// Called with the identity once the waiter may retry; must stay callable until then.
		using Notify = std::function<void(std::uint64_t)>;

		// Registers a waiter, then calls a_retry once more: the resource may have freed before the
		// waiter was visible. Returns 0 when that retry succeeded, else the identity to suspend on.
		[[nodiscard]] std::uint64_t Wait(Notify a_notify, const std::function<bool()>& a_retry);

		// Notifies the oldest waiter still suspended; false when there is none.
		bool WakeOne();
		void WakeAll();

	private:
		struct Waiter
		{
			std::uint64_t identity{ 0 };
			Notify notify;
			// Cleared by whichever comes first: the retry succeeding or a wake.
			std::atomic<bool> active{ true };
		};

		oneapi::tbb::concurrent_queue<std::shared_ptr<Waiter>> _waiters;
	};
}
