#include "capture.hpp"
#include "../mem/guest.hpp"

#include <crt.hpp>
#include <mutex.hpp>

namespace
{
	capture::rec ring[capture::capacity];
	std::size_t head = 0;
	std::uint64_t dropped_count = 0;
	std::uint64_t seq_counter = 0;
	cstd::mutex mtx;
}

bool capture::push(const std::uint64_t cr3_flags, const std::uint64_t va,
                   const std::uint64_t gpa, const std::uint64_t rip,
                   const std::uint32_t why, const void* const data) noexcept
{
	cstd::lock_guard guard{ mtx };

	if (head >= capacity)
	{
		dropped_count++;
		return false;
	}

	auto& slot = ring[head];

	slot.seq = ++seq_counter;
	slot.cr3 = cr3_flags;
	slot.va = va;
	slot.gpa = gpa;
	slot.rip = rip;
	slot.why = why;
	slot.flags = 0;

	if (data)
		cstd::crt::memcpy(slot.data, data, page_bytes);
	else
		cstd::crt::memset(slot.data, 0, page_bytes);

	head++;

	return true;
}

std::size_t capture::flush(const cr3 dest_cr3, const std::uintptr_t dest_va,
                           const std::uint16_t count) noexcept
{
	cstd::lock_guard guard{ mtx };

	if (head == 0)
		return 0;

	const auto n = count < head ? count : head;
	if (!mem::guest::write_virt(dest_cr3, dest_va, ring, n * sizeof(rec)))
		return 0;

	for (std::size_t i = n; i < head; i++)
		ring[i - n] = ring[i];

	head -= n;

	return n;
}

std::size_t capture::count() noexcept
{
	cstd::lock_guard guard{ mtx };
	return head;
}

std::size_t capture::captured_bytes() noexcept
{
	cstd::lock_guard guard{ mtx };
	return head * sizeof(rec);
}

std::uint64_t capture::dropped() noexcept
{
	cstd::lock_guard guard{ mtx };
	return dropped_count;
}

void capture::reset() noexcept
{
	cstd::lock_guard guard{ mtx };
	head = 0;
}
