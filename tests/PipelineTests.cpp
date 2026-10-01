#include "Pipeline/MpscQueue.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

TEST(MpscQueue, DeliversEveryItemFromManyProducersExactlyOnce)
{
	constexpr int kProducers = 8;
	constexpr int kPerProducer = 20000;
	FasterNGIO::Pipeline::MpscQueue<int> queue;
	std::vector<std::thread> producers;
	for (int p = 0; p < kProducers; ++p) {
		producers.emplace_back([&, p] {
			for (int i = 0; i < kPerProducer; ++i) {
				queue.Push(p * kPerProducer + i);
			}
		});
	}

	std::vector<int> seen(kProducers * kPerProducer, 0);
	std::vector<int> lastFromProducer(kProducers, -1);
	int received = 0;
	while (received < kProducers * kPerProducer) {
		if (auto value = queue.TryPop()) {
			++seen[*value];
			// Per-producer FIFO order is preserved.
			const auto producer = *value / kPerProducer;
			EXPECT_GT(*value, lastFromProducer[producer]);
			lastFromProducer[producer] = *value;
			++received;
		}
	}
	for (auto& producer : producers) {
		producer.join();
	}
	EXPECT_FALSE(queue.TryPop().has_value());
	for (const auto count : seen) {
		ASSERT_EQ(count, 1);
	}
}
