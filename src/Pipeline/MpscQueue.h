#pragma once

#include <atomic>
#include <optional>
#include <utility>

namespace FasterNGIO::Pipeline
{
	// Lock-free multi-producer / single-consumer queue (Vyukov's intrusive design with a stub
	// node). Push is wait-free for producers; TryPop must only be called by the one consumer.
	// TryPop can report empty while a producer is between its two stores; the consumer simply
	// sees that element on its next pass.
	template <class T>
	class MpscQueue
	{
	public:
		MpscQueue() :
			_head(&_stub), _tail(&_stub) {}

		MpscQueue(const MpscQueue&) = delete;
		MpscQueue& operator=(const MpscQueue&) = delete;

		~MpscQueue()
		{
			while (TryPop()) {
			}
		}

		void Push(T a_value)
		{
			auto* node = new Node{ std::move(a_value) };
			auto* previous = _head.exchange(node, std::memory_order_acq_rel);
			previous->next.store(node, std::memory_order_release);
		}

		[[nodiscard]] std::optional<T> TryPop()
		{
			auto* tail = _tail;
			auto* next = tail->next.load(std::memory_order_acquire);
			if (tail == &_stub) {
				if (!next) {
					return std::nullopt;
				}
				_tail = next;
				tail = next;
				next = next->next.load(std::memory_order_acquire);
			}
			if (next) {
				_tail = next;
				return Take(tail);
			}
			if (tail != _head.load(std::memory_order_acquire)) {
				return std::nullopt;
			}
			// tail is the last real node: re-insert the stub behind it so it can be detached.
			_stub.next.store(nullptr, std::memory_order_relaxed);
			auto* previous = _head.exchange(&_stub, std::memory_order_acq_rel);
			previous->next.store(&_stub, std::memory_order_release);
			next = tail->next.load(std::memory_order_acquire);
			if (next) {
				_tail = next;
				return Take(tail);
			}
			return std::nullopt;
		}

	private:
		struct Node
		{
			std::optional<T> value;
			std::atomic<Node*> next{ nullptr };
		};

		[[nodiscard]] static std::optional<T> Take(Node* a_node)
		{
			auto value = std::move(a_node->value);
			delete a_node;
			return value;
		}

		std::atomic<Node*> _head;
		Node* _tail;
		Node _stub;
	};
}
