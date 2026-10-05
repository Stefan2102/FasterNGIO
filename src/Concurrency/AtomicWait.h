#pragma once

#include <atomic>

namespace FasterNGIO::Concurrency
{
	// Sleeps until a_value satisfies a_done. Whoever changes a_value must notify afterwards.
	template <class T, class Done>
	void WaitUntil(const std::atomic<T>& a_value, Done&& a_done)
	{
		for (auto value = a_value.load(std::memory_order_acquire); !a_done(value); value = a_value.load(std::memory_order_acquire)) {
			a_value.wait(value, std::memory_order_acquire);
		}
	}
}
