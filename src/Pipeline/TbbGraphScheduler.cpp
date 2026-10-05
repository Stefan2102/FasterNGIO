#include "Pipeline/TbbGraphScheduler.h"

#include "Concurrency/AtomicWait.h"

#include <oneapi/tbb/task_group.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace FasterNGIO::Pipeline
{
	namespace
	{
		struct Identity
		{
		};

		class TbbScope final : public org::async::TaskScope
		{
		public:
			explicit TbbScope(std::shared_ptr<const Identity> a_owner) :
				owner(std::move(a_owner)) {}

			[[nodiscard]] bool StopRequested() const noexcept override { return stopped.load(std::memory_order_acquire); }
			void Cancel() noexcept override { stopped.store(true, std::memory_order_release); }

			void Wait() const override
			{
				// Delayed tasks land in the group later; wait until none are pending first.
				Concurrency::WaitUntil(delayed, [](std::uint64_t a_pending) { return a_pending == 0; });
				group->wait();
			}

			std::shared_ptr<const Identity> owner;
			// Held by pointer: task_group's destructor may throw, TaskScope's must not.
			std::unique_ptr<oneapi::tbb::task_group> group = std::make_unique<oneapi::tbb::task_group>();
			mutable std::atomic<std::uint64_t> delayed{ 0 };
			std::atomic<bool> stopped{ false };
		};

		class TbbGraphScheduler final : public org::async::GraphScheduler
		{
		public:
			org::async::Scope CreateScope(std::string_view) override { return std::make_shared<TbbScope>(_identity); }

			bool Dispatch(const org::async::Scope& a_scope, org::async::TaskClass, org::async::TaskDispatch, std::string_view, Task a_task,
				org::async::TaskTraceMetadata) override
			{
				auto scope = Resolve(a_scope);
				if (!scope || !a_task) {
					return false;
				}
				Run(scope, std::move(a_task));
				return true;
			}

			bool DispatchAfter(const org::async::Scope& a_scope, std::chrono::steady_clock::duration a_delay, org::async::TaskClass, std::string_view,
				Task a_task) override
			{
				auto scope = Resolve(a_scope);
				if (!scope || !a_task) {
					return false;
				}
				scope->delayed.fetch_add(1, std::memory_order_acq_rel);
				std::thread([scope, delay = a_delay, task = std::move(a_task)]() mutable {
					std::this_thread::sleep_for(delay);
					Run(scope, std::move(task));
					scope->delayed.fetch_sub(1, std::memory_order_acq_rel);
					scope->delayed.notify_all();
				}).detach();
				return true;
			}

		private:
			[[nodiscard]] std::shared_ptr<TbbScope> Resolve(const org::async::Scope& a_scope) const
			{
				auto scope = std::dynamic_pointer_cast<TbbScope>(a_scope);
				return scope && scope->owner == _identity ? scope : nullptr;
			}

			static void Run(const std::shared_ptr<TbbScope>& a_scope, Task a_task)
			{
				a_scope->group->run([scope = a_scope, task = std::move(a_task)] {
					const std::weak_ptr<TbbScope> weak = scope;
					task(org::async::TaskContext{ [weak] {
						const auto locked = weak.lock();
						return !locked || locked->StopRequested();
					} });
				});
			}

			const std::shared_ptr<const Identity> _identity = std::make_shared<Identity>();
		};
	}

	std::shared_ptr<org::async::GraphScheduler> MakeTbbGraphScheduler()
	{
		return std::make_shared<TbbGraphScheduler>();
	}
}
