#pragma once

#include "Rejection/RejectionConfig.h"
#include "Rejection/WorldIndex.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace FasterNGIO::Gpu
{
	// One blade's query: the bottom of its test segment (x, y, z - depth) and its capsule radius.
	struct Query
	{
		float x{ 0.0f };
		float y{ 0.0f };
		float bottomZ{ 0.0f };
		float radius{ 0.0f };
	};

	// A batch of queries (one cell's) handed to the render thread. Its result is published once,
	// lock-free: Complete() flips with release semantics after Hits() is written, and every
	// subscriber registered before completion runs exactly once on the render thread.
	class TraceJob
	{
	public:
		explicit TraceJob(std::vector<Query> a_queries);
		~TraceJob();
		TraceJob(const TraceJob&) = delete;
		TraceJob& operator=(const TraceJob&) = delete;

		[[nodiscard]] std::span<const Query> Queries() const noexcept { return _queries; }
		// One 0/1 flag per query; valid once Complete() is true and Failed() is false.
		[[nodiscard]] std::span<const std::uint32_t> Hits() const noexcept { return _hits; }
		[[nodiscard]] bool Complete() const noexcept { return _complete.load(std::memory_order_acquire); }
		[[nodiscard]] bool Failed() const noexcept { return _failed.load(std::memory_order_acquire); }

		// Registers a completion callback. Returns false (and drops the callback) when the job has
		// already completed; the caller observes Complete() instead.
		bool Subscribe(std::function<void()> a_callback);

		// Render thread only: publishes the result (or a failure) and runs the subscribers.
		void Finish(std::span<const std::uint32_t> a_hits, bool a_failed) noexcept;

		// Releases the queries once traced; they are only needed by the GPU.
		void ReleaseQueries() noexcept;

	private:
		struct Subscriber
		{
			std::function<void()> callback;
			Subscriber* next{ nullptr };
		};

		[[nodiscard]] static Subscriber* ClosedList() noexcept { return reinterpret_cast<Subscriber*>(std::uintptr_t{ 1 }); }

		std::vector<Query> _queries;
		std::vector<std::uint32_t> _hits;
		std::atomic<bool> _complete{ false };
		std::atomic<bool> _failed{ false };
		std::atomic<Subscriber*> _subscribers{ nullptr };
	};

	struct GpuRejectorDesc
	{
		// Directory holding GrassRejection.hlsl and Shared/.
		std::filesystem::path shaderDirectory;
		// Compiled-shader cache; empty disables the disk cache.
		std::filesystem::path shaderCacheDirectory;
		Rejection::QueryMode mode{ Rejection::QueryMode::Capsule };
		// depth + height: the length of every query segment.
		float segmentLength{ 155.0f };
		// Widest query radius any grass type uses; collision AABBs are grown by it.
		float maxQueryRadius{ 0.0f };
		std::uint32_t maxQueriesPerFrame{ 1u << 22 };
		bool debugLayer{ false };
	};

	struct GpuRejectorStats
	{
		std::uint64_t models{ 0 };
		std::uint64_t instances{ 0 };
		std::uint64_t blasBytes{ 0 };
		std::uint64_t modelBytes{ 0 };
		std::uint64_t framesExecuted{ 0 };
		std::uint64_t queries{ 0 };
		double worldBuildSeconds{ 0.0 };
	};

	// Tests grass queries against a world's collision with DXR. All GPU work runs on one render
	// thread that owns an OpenRenderGraph persistent host. Callers never block on it: work is
	// posted through a lock-free inbox and results come back through each TraceJob.
	class GpuRejector
	{
	public:
		// Creates the device and pipeline; throws if the adapter cannot run DXR.
		explicit GpuRejector(GpuRejectorDesc a_desc);
		~GpuRejector();
		GpuRejector(const GpuRejector&) = delete;
		GpuRejector& operator=(const GpuRejector&) = delete;

		// Posts the world (BLAS per model, one TLAS). Jobs posted before the world is built wait
		// on the render thread. The index must outlive the rejector.
		void PostWorld(std::shared_ptr<const Rejection::WorldIndex> a_world);

		// Lock-free; callable from any thread.
		void Post(std::shared_ptr<TraceJob> a_job);

		// Snapshot of monotonically updated counters.
		[[nodiscard]] GpuRejectorStats Stats() const;

	private:
		class Impl;
		std::unique_ptr<Impl> _impl;
	};
}
