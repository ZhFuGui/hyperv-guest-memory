#include "capture.hpp"
#include "../mem/guest.hpp"
#include "../vcpu/vcpu.hpp"

#include <atomic>
#include <crt.hpp>

namespace
{
	struct alignas(64) ring
	{
		alignas(64) std::atomic<std::uint64_t> head{};
		alignas(64) std::atomic<std::uint64_t> tail{};
	};

	// Trivial storage needs no CRT initializer; keep it separate from atomic indices.
	capture::rec records[capture::max_cpus][capture::per_cpu_capacity];
	constinit ring rings[capture::max_cpus]{};
	constinit std::atomic<std::uint64_t> sequence{};
	constinit std::atomic<std::uint64_t> lost{};
	constinit std::atomic_flag consuming = ATOMIC_FLAG_INIT;
	std::size_t next_ring = 0;
	static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

	struct consumer_guard
	{
		bool acquired = !consuming.test_and_set(std::memory_order_acquire);
		~consumer_guard() { if (acquired) consuming.clear(std::memory_order_release); }
	};
}

void capture::note_drop() noexcept { lost.fetch_add(1, std::memory_order_relaxed); }

bool capture::push(const std::uint64_t cr3_flags, const std::uint64_t va,
                   const std::uint64_t gpa, const std::uint64_t rip,
                   const std::uint32_t why, const void* const data) noexcept
{
	const auto id = vcpu::curr_id();
	if (id >= max_cpus || !data)
	{
		note_drop();
		return false;
	}

	auto& r = rings[id];
	const auto head = r.head.load(std::memory_order_relaxed);
	const auto tail = r.tail.load(std::memory_order_acquire);
	if (head - tail >= per_cpu_capacity)
	{
		note_drop();
		return false;
	}

	auto& slot = records[id][head % per_cpu_capacity];
	slot.cr3 = cr3_flags;
	slot.va = va;
	slot.gpa = gpa;
	slot.rip = rip;
	slot.why = why;
	slot.flags = static_cast<std::uint32_t>(id);
	cstd::crt::memcpy(slot.data, data, page_bytes);
	slot.seq = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
	r.head.store(head + 1, std::memory_order_release);
	return true;
}

std::size_t capture::flush(const cr3 dest_cr3, const std::uintptr_t dest_va,
                           const std::uint16_t count) noexcept
{
	consumer_guard guard;
	if (!guard.acquired || !dest_va || !count)
		return 0;

	const auto limit = count < max_flush_records ? count : max_flush_records;
	if (dest_va > UINTPTR_MAX - limit * sizeof(rec))
		return 0;

	std::size_t written = 0;
	std::size_t empty = 0;
	while (written < limit && empty < max_cpus)
	{
		const auto id = next_ring;
		auto& r = rings[id];
		next_ring = (next_ring + 1) % max_cpus;
		const auto tail = r.tail.load(std::memory_order_relaxed);
		const auto head = r.head.load(std::memory_order_acquire);
		if (tail == head)
		{
			++empty;
			continue;
		}

		empty = 0;
		const auto& slot = records[id][tail % per_cpu_capacity];
		if (!mem::guest::write_virt(dest_cr3, dest_va + written * sizeof(rec), &slot, sizeof(rec)))
			break;

		r.tail.store(tail + 1, std::memory_order_release);
		++written;
	}
	return written;
}

std::size_t capture::count() noexcept
{
	std::size_t total = 0;
	for (const auto& r : rings)
	{
		const auto tail = r.tail.load(std::memory_order_acquire);
		const auto size = r.head.load(std::memory_order_acquire) - tail;
		total += size < per_cpu_capacity ? static_cast<std::size_t>(size) : per_cpu_capacity;
	}
	return total;
}

std::size_t capture::captured_bytes() noexcept { return count() * sizeof(rec); }
std::uint64_t capture::dropped() noexcept { return lost.load(std::memory_order_relaxed); }

bool capture::reset() noexcept
{
	consumer_guard guard;
	if (!guard.acquired)
		return false;
	for (auto& r : rings)
		r.tail.store(r.head.load(std::memory_order_acquire), std::memory_order_release);
	return true;
}
