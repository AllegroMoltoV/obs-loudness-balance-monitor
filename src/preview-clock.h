#pragma once
#include <atomic>
#include <cstdint>

namespace lbm {
template<typename Clock> uint64_t packet_idle_ns(const std::atomic<uint64_t> &last_packet, Clock now)
{
	// Sequence the atomic read before the clock read: a callback may update it between them.
	const uint64_t last = last_packet.load(std::memory_order_acquire);
	const uint64_t current = now();
	return current >= last ? current - last : 0;
}
} // namespace lbm
