#include "Pipeline/SuspensionWaiters.h"

#include <ORGModuleServices/Async/SuspensionIdentity.h>

namespace FasterNGIO::Pipeline
{
	std::uint64_t SuspensionWaiters::Wait(Notify a_notify, const std::function<bool()>& a_retry)
	{
		auto waiter = std::make_shared<Waiter>();
		waiter->identity = org::async::AllocateArtifactSuspensionIdentity();
		waiter->notify = std::move(a_notify);
		_waiters.push(waiter);
		if (a_retry()) {
			waiter->active.store(false, std::memory_order_release);
			return 0;
		}
		return waiter->identity;
	}

	bool SuspensionWaiters::WakeOne()
	{
		std::shared_ptr<Waiter> waiter;
		while (_waiters.try_pop(waiter)) {
			if (waiter->active.exchange(false, std::memory_order_acq_rel)) {
				waiter->notify(waiter->identity);
				return true;
			}
		}
		return false;
	}

	void SuspensionWaiters::WakeAll()
	{
		while (WakeOne()) {
		}
	}
}
