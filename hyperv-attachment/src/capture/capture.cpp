#include "capture.hpp"
#include "../mem/guest.hpp"
#include "../vcpu/vcpu.hpp"

#include <crt.hpp>
#include <intrin.h>

namespace
{
	struct ring
	{
		capture::rec entries[capture::per_cpu_capacity];
		volatile std::uint32_t head;
		volatile std::uint64_t dropped;
	};

	// One ring per CPU. No shared state, no locks: the owner CPU is the only
	// writer, flush only reads [0, head) and resets head afterwards.
	ring rings[capture::max_cpus];
	volatile std::uint64_t seq_counter = 1;

	ring& ring_for_this_cpu() noexcept
	{
		const auto id = vcpu::curr_id();
		return rings[id < capture::max_cpus ? id : 0];
	}
}

bool capture::push(const std::uint64_t cr3_flags, const std::uint64_t va,
                   const std::uint64_t gpa, const std::uint64_t rip,
                   const std::uint32_t why, const void* const data) noexcept
{
	auto& r = ring_for_this_cpu();

	if (r.head >= per_cpu_capacity)
	{
		r.dropped = r.dropped + 1;
		return false;
	}

	auto& slot = r.entries[r.head];

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

	slot.seq = seq_counter++;

	// publish only after the record is complete
	_ReadWriteBarrier();
	r.head = r.head + 1;

	return true;
}

std::size_t capture::flush(const cr3 dest_cr3, const std::uintptr_t dest_va,
                           const std::uint16_t count) noexcept
{
	std::size_t written = 0;
	std::uintptr_t dest = dest_va;

	for (std::size_t i = 0; i < max_cpus && written < count; i++)
	{
		auto& r = rings[i];

		const auto head = static_cast<std::size_t>(r.head);
		if (head == 0)
			continue;

		auto take = head;
		if (take > count - written)
			take = count - written;

		const auto bytes = take * sizeof(rec);
		if (!mem::guest::write_virt(dest_cr3, dest, r.entries, bytes))
			continue;

		// drop the records that were taken; if the owner pushed meanwhile it
		// keeps writing above `take`, so moving the remainder down is safe
		for (std::size_t k = take; k < head; k++)
			r.entries[k - take] = r.entries[k];

		r.head = static_cast<std::uint32_t>(head - take);

		written += take;
		dest += bytes;
	}

	return written;
}

std::size_t capture::count() noexcept
{
	std::size_t total = 0;

	for (std::size_t i = 0; i < max_cpus; i++)
		total += rings[i].head;

	return total;
}

std::size_t capture::captured_bytes() noexcept
{
	return count() * sizeof(rec);
}

std::uint64_t capture::dropped() noexcept
{
	std::uint64_t total = 0;

	for (std::size_t i = 0; i < max_cpus; i++)
		total += rings[i].dropped;

	return total;
}

void capture::reset() noexcept
{
	for (std::size_t i = 0; i < max_cpus; i++)
		rings[i].head = 0;
}
