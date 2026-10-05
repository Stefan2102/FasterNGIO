#pragma once

#include "Concurrency/MpscQueue.h"
#include "Pipeline/SuspensionWaiters.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

namespace FasterNGIO::Pipeline
{
	// What happened to the files one submitter (a world) handed over.
	struct WriteTally
	{
		std::atomic<std::uint64_t> written{ 0 };
		std::atomic<std::uint64_t> failed{ 0 };
	};

	// A few threads that only write finished cache files, so the cores placing and tracing grass
	// never wait on the filesystem. File creation does not scale with threads on Windows (NTFS and
	// the antivirus filter serialize it, and more writers only contend in the kernel), so one
	// writer by default (--writers) keeps the disk busy while everything else computes.
	//
	// Lock-free: each writer owns an MPSC queue and sleeps on an atomic. Producers never block:
	// AdmitOrWait reports when the backlog is over budget, and the caller suspends in its graph
	// until the backlog drains.
	class FileWriterPool
	{
	public:
		FileWriterPool(std::uint32_t a_threads, std::uint64_t a_maxPendingBytes);
		// Writes everything still queued, then stops the threads.
		~FileWriterPool();
		FileWriterPool(const FileWriterPool&) = delete;
		FileWriterPool& operator=(const FileWriterPool&) = delete;

		// Queues a file; a_tally counts its outcome. The folder must exist.
		void Submit(std::filesystem::path a_path, std::vector<std::uint8_t> a_bytes, std::shared_ptr<WriteTally> a_tally);

		// 0 when the backlog is under budget and the caller may produce another file. Otherwise an
		// identity to suspend on: a_notify(identity) is called once the backlog has drained enough.
		[[nodiscard]] std::uint64_t AdmitOrWait(SuspensionWaiters::Notify a_notify);

		// Sleeps until every queued file has been written.
		void Drain();

		[[nodiscard]] std::uint64_t PendingFiles() const { return _pendingFiles.load(std::memory_order_acquire); }

		// Files written (or failed) and the seconds the threads spent in the filesystem, summed.
		[[nodiscard]] std::uint64_t FilesDone() const { return _filesDone.load(std::memory_order_relaxed); }
		[[nodiscard]] double BusySeconds() const { return static_cast<double>(_busyNanoseconds.load(std::memory_order_relaxed)) * 1.0e-9; }

	private:
		struct Job
		{
			std::filesystem::path path;
			std::vector<std::uint8_t> bytes;
			std::shared_ptr<WriteTally> tally;
		};

		struct Slot
		{
			Concurrency::MpscQueue<Job> queue;
			std::atomic<std::uint32_t> signal{ 0 };
		};

		[[nodiscard]] bool UnderBudget() const { return _pendingBytes.load(std::memory_order_acquire) < _maxPendingBytes; }
		void Loop(Slot& a_slot);
		void Write(Job& a_job);

		const std::uint64_t _maxPendingBytes;
		std::vector<std::unique_ptr<Slot>> _slots;
		std::atomic<std::uint32_t> _next{ 0 };
		std::atomic<std::uint64_t> _pendingFiles{ 0 };
		std::atomic<std::uint64_t> _pendingBytes{ 0 };
		std::atomic<bool> _stopping{ false };
		std::atomic<std::uint64_t> _filesDone{ 0 };
		std::atomic<std::uint64_t> _busyNanoseconds{ 0 };
		SuspensionWaiters _waiters;
		std::vector<std::thread> _threads;
	};
}
