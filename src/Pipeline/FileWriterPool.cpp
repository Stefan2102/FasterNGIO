#include "Pipeline/FileWriterPool.h"

#include "Concurrency/AtomicWait.h"
#include "Platform/WholeFile.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <exception>

namespace FasterNGIO::Pipeline
{
	FileWriterPool::FileWriterPool(std::uint32_t a_threads, std::uint64_t a_maxPendingBytes) :
		_maxPendingBytes(a_maxPendingBytes)
	{
		const auto threads = (std::max)(a_threads, 1u);
		for (std::uint32_t i = 0; i < threads; ++i) {
			_slots.push_back(std::make_unique<Slot>());
		}
		for (auto& slot : _slots) {
			_threads.emplace_back([this, &slot = *slot] { Loop(slot); });
		}
	}

	FileWriterPool::~FileWriterPool()
	{
		Drain();
		_stopping.store(true, std::memory_order_release);
		for (auto& slot : _slots) {
			slot->signal.fetch_add(1, std::memory_order_release);
			slot->signal.notify_one();
		}
		for (auto& thread : _threads) {
			thread.join();
		}
	}

	void FileWriterPool::Submit(std::filesystem::path a_path, std::vector<std::uint8_t> a_bytes, std::shared_ptr<WriteTally> a_tally)
	{
		_pendingBytes.fetch_add(a_bytes.size(), std::memory_order_acq_rel);
		_pendingFiles.fetch_add(1, std::memory_order_acq_rel);
		auto& slot = *_slots[_next.fetch_add(1, std::memory_order_relaxed) % _slots.size()];
		slot.queue.Push(Job{ std::move(a_path), std::move(a_bytes), std::move(a_tally) });
		// After the push: a writer that read the old signal value is guaranteed to see the job.
		slot.signal.fetch_add(1, std::memory_order_release);
		slot.signal.notify_one();
	}

	std::uint64_t FileWriterPool::AdmitOrWait(SuspensionWaiters::Notify a_notify)
	{
		if (UnderBudget()) {
			return 0;
		}
		return _waiters.Wait(std::move(a_notify), [this] { return UnderBudget(); });
	}

	void FileWriterPool::Drain()
	{
		Concurrency::WaitUntil(_pendingFiles, [](std::uint64_t a_pending) { return a_pending == 0; });
	}

	void FileWriterPool::Loop(Slot& a_slot)
	{
		while (true) {
			const auto seen = a_slot.signal.load(std::memory_order_acquire);
			bool wrote = false;
			while (auto job = a_slot.queue.TryPop()) {
				Write(*job);
				wrote = true;
			}
			if (!wrote && _stopping.load(std::memory_order_acquire)) {
				return;
			}
			a_slot.signal.wait(seen, std::memory_order_acquire);
		}
	}

	void FileWriterPool::Write(Job& a_job)
	{
		const auto begin = std::chrono::steady_clock::now();
		try {
			Platform::WriteWholeFile(a_job.path, a_job.bytes);
			a_job.tally->written.fetch_add(1, std::memory_order_relaxed);
		} catch (const std::exception& e) {
			spdlog::error("{}", e.what());
			a_job.tally->failed.fetch_add(1, std::memory_order_relaxed);
		}
		_busyNanoseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count()),
			std::memory_order_relaxed);
		_filesDone.fetch_add(1, std::memory_order_relaxed);
		_pendingBytes.fetch_sub(a_job.bytes.size(), std::memory_order_acq_rel);
		a_job = Job{};
		const auto left = _pendingFiles.fetch_sub(1, std::memory_order_acq_rel) - 1;
		if (left == 0) {
			// Nothing else would wake the remaining waiters.
			_waiters.WakeAll();
			_pendingFiles.notify_all();
		} else if (UnderBudget()) {
			_waiters.WakeOne();
		}
	}
}
