#include "batch.hpp"
#include "../../mem/guest.hpp"
#include "../../vcpu/vcpu.hpp"

namespace
{
	bool valid_range(std::uint64_t address, std::uint64_t size) noexcept
	{
		if (!size)
			return true;
		if (!address || size - 1 > UINT64_MAX - address)
			return false;
		const auto top = address >> 47;
		return (top == 0 || top == 0x1FFFF) &&
		       ((address + size - 1) >> 47) == top;
	}

	bool overlaps(std::uint64_t a, std::uint64_t a_size,
	              std::uint64_t b, std::uint64_t b_size) noexcept
	{
		return a_size && b_size && (a <= b ? b - a < a_size : a - b < b_size);
	}

	struct release_workspace
	{
		hc::batch::workspace& work;
		~release_workspace() { work.busy.store(false, cstd::memory_order::release); }
	};
}

bool hc::batch::read(const std::uint64_t source_cr3, const std::uintptr_t items_va,
                     const std::uint64_t count, const std::uintptr_t mask_va) noexcept
{
	// Validate before narrowing count. Match the existing four-level walker:
	// 48-bit canonical VAs, CR3 table base in bits 47:12, low PCID bits retained.
	if (!count || count > batch_max_items ||
	    !(source_cr3 & 0x0000FFFFFFFFF000ULL) || (source_cr3 >> 48))
		return false;
	const auto items_bytes = count * sizeof(batch_read_item);
	if (!valid_range(items_va, items_bytes) || !valid_range(mask_va, sizeof(std::uint64_t)) ||
	    (mask_va & 7) || overlaps(items_va, items_bytes, mask_va, sizeof(std::uint64_t)))
		return false;

	auto* const work = vcpu::curr().batch_work;
	if (!work || work->busy.exchange(true, cstd::memory_order::acquire))
		return false;
	const release_workspace release{*work};
	const auto caller = arch::guest_cr3();
	const auto slat = arch::slat_cr3();
	if (!mem::guest::read_virt(caller, slat, items_va, work->items, items_bytes))
		return false;

	std::uint64_t total = 0;
	for (std::uint64_t i = 0; i < count; ++i)
	{
		const auto& item = work->items[i];
		if (item.size > batch_max_bytes - total ||
		    !valid_range(item.source_va, item.size) || !valid_range(item.destination_va, item.size) ||
		    overlaps(item.destination_va, item.size, items_va, items_bytes) ||
		    overlaps(item.destination_va, item.size, mask_va, sizeof(std::uint64_t)))
			return false;
		total += item.size;
		for (std::uint64_t j = 0; j < i; ++j)
			if (overlaps(item.destination_va, item.size, work->items[j].destination_va, work->items[j].size))
				return false;
	}

	std::uint64_t mask = 0;
	const cr3 source{.flags = source_cr3};
	for (std::uint64_t i = 0; i < count; ++i)
	{
		const auto& item = work->items[i];
		if (mem::guest::copy_virt(slat, source, item.source_va, caller, item.destination_va, item.size))
			mask |= 1ULL << i;
	}
	// No pointers or pending work survive this return. If publication fails,
	// RAX=0 invalidates every destination even if some copies already happened.
	return mem::guest::write_virt(caller, slat, mask_va, &mask, sizeof(mask));
}
