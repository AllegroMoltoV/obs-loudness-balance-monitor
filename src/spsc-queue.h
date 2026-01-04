#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace lbm {

// Lock-free Single-Producer Single-Consumer Queue
// Used for transferring audio frames from audio callback to worker thread
template<typename T, size_t Capacity> class SPSCQueue {
public:
	SPSCQueue() = default;
	~SPSCQueue() = default;

	// Non-copyable, non-movable
	SPSCQueue(const SPSCQueue &) = delete;
	SPSCQueue &operator=(const SPSCQueue &) = delete;
	SPSCQueue(SPSCQueue &&) = delete;
	SPSCQueue &operator=(SPSCQueue &&) = delete;

	// Try to push an item (producer side)
	// Returns false if queue is full
	bool try_push(const T &item)
	{
		const size_t current_head = head_.load(std::memory_order_relaxed);
		const size_t next_head = (current_head + 1) % Capacity;

		if (next_head == tail_.load(std::memory_order_acquire)) {
			return false; // Queue full
		}

		buffer_[current_head] = item;
		head_.store(next_head, std::memory_order_release);
		return true;
	}

	// Try to pop an item (consumer side)
	// Returns false if queue is empty
	bool try_pop(T &item)
	{
		const size_t current_tail = tail_.load(std::memory_order_relaxed);

		if (current_tail == head_.load(std::memory_order_acquire)) {
			return false; // Queue empty
		}

		item = buffer_[current_tail];
		tail_.store((current_tail + 1) % Capacity, std::memory_order_release);
		return true;
	}

	// Approximate size (may not be exact due to concurrent access)
	size_t size_approx() const
	{
		const size_t head = head_.load(std::memory_order_relaxed);
		const size_t tail = tail_.load(std::memory_order_relaxed);
		return (head >= tail) ? (head - tail) : (Capacity - tail + head);
	}

	bool empty() const { return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_relaxed); }

	void clear()
	{
		head_.store(0, std::memory_order_relaxed);
		tail_.store(0, std::memory_order_relaxed);
	}

private:
	std::array<T, Capacity> buffer_;
	std::atomic<size_t> head_{0}; // Producer writes here
	std::atomic<size_t> tail_{0}; // Consumer reads here
};

} // namespace lbm
