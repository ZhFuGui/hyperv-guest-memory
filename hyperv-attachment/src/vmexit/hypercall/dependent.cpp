#include "dependent.hpp"
#include "batch.hpp"
#include "../../mem/guest.hpp"
#include "../../vcpu/vcpu.hpp"
#include <dependent.hpp>

namespace
{
	using kind = hc::dependent_read_kind;
	using status = hc::dependent_read_status;
	struct state { hc::dependent_read_item items[hc::dependent_max_items]; hc::dependent_read_result result; };
	static_assert(sizeof(state) <= sizeof(hc::batch::workspace::storage));

	bool range(std::uint64_t va, std::uint64_t size) noexcept
	{
		if (!va || !size || size - 1 > UINT64_MAX - va) return false;
		const auto top = va >> 47;
		return (top == 0 || top == 0x1FFFF) && ((va + size - 1) >> 47) == top;
	}
	bool overlaps(std::uint64_t a, std::uint64_t na, std::uint64_t b, std::uint64_t nb) noexcept
	{
		return na && nb && (a <= b ? b - a < na : a - b < nb);
	}
	bool relative(std::uint64_t base, std::uint64_t offset, std::uint64_t& va) noexcept
	{
		// Avoid signed overflow, including INT64_MIN, and reject unsigned wrapping.
		if (!(offset >> 63)) {
			if (offset > UINT64_MAX - base) return false;
			va = base + offset;
		} else {
			const auto magnitude = ~offset + 1;
			if (magnitude > base) return false;
			va = base - magnitude;
		}
		return true;
	}
}

bool hc::dependent::read(std::uint64_t source_cr3, std::uintptr_t items_va,
                         std::uint64_t count, std::uintptr_t result_va) noexcept
{
	if (!count || count > dependent_max_items ||
	    !(source_cr3 & 0x0000FFFFFFFFF000ULL) || (source_cr3 >> 48)) return false;
	const auto items_bytes = count * sizeof(dependent_read_item);
	if (!range(items_va, items_bytes) || !range(result_va, sizeof(dependent_read_result)) ||
	    overlaps(items_va, items_bytes, result_va, sizeof(dependent_read_result))) return false;
	const auto cpu = vcpu::curr_id();
	if (cpu >= vcpu::count()) return false;
	auto* const work = vcpu::get(cpu).batch_work;
	if (!work || work->busy.exchange(true, cstd::memory_order::acquire)) return false;
	struct release { hc::batch::workspace& w; ~release() { w.busy.store(false, cstd::memory_order::release); } } done{*work};
	const auto caller = arch::guest_cr3(), slat = arch::slat_cr3();
	if (!caller.address_of_page_directory || !slat.address_of_page_directory) return false;
	auto* const s = new(work->storage) state{};
	if (!mem::guest::read_virt(caller, slat, items_va, s->items, items_bytes)) return false;

	// Validate the complete snapshot before any payload write. Backward pointer
	// references make the execution finite and require no sorting or recursion.
	std::uint64_t total = 0;
	for (std::uint64_t i = 0; i < count; ++i) {
		const auto& item = s->items[i];
		if (item.reserved || (item.kind != kind::pointer && item.kind != kind::copy) ||
		    !item.size || item.size > dependent_max_bytes - total ||
		    (item.parent == dependent_direct ? !range(item.address, item.size)
		     : item.parent >= i || s->items[item.parent].kind != kind::pointer)) return false;
		total += item.size;
		if (item.kind == kind::pointer) {
			if (item.size != 8 || item.destination_va) return false;
		} else {
			if (!range(item.destination_va, item.size) ||
			    overlaps(item.destination_va, item.size, items_va, items_bytes) ||
			    overlaps(item.destination_va, item.size, result_va, sizeof(dependent_read_result))) return false;
			for (std::uint64_t j = 0; j < i; ++j)
				if (s->items[j].kind == kind::copy && overlaps(item.destination_va, item.size,
				    s->items[j].destination_va, s->items[j].size)) return false;
		}
	}

	const cr3 source{.flags = source_cr3};
	auto& out = s->result;
	for (std::uint64_t i = 0; i < count; ++i) {
		const auto& item = s->items[i];
		auto va = item.address;
		if (item.parent != dependent_direct) {
			if (out.status[item.parent] != status::success) { out.status[i] = status::dependency_failed; continue; }
			if (!relative(out.pointers[item.parent], item.address, va) || !range(va, item.size)) {
				out.status[i] = status::invalid_address; continue;
			}
		}
		out.status[i] = status::transfer_failed;
		if (item.kind == kind::pointer) {
			std::uint64_t pointer = 0;
			if (!mem::guest::read_virt(source, slat, va, &pointer, sizeof(pointer))) continue;
			if (!range(pointer, 1)) { out.status[i] = status::invalid_address; continue; }
			out.pointers[i] = pointer;
		} else if (!mem::guest::copy_virt(slat, source, va, caller, item.destination_va, item.size)) continue;
		out.status[i] = status::success;
	}
	// Publication failure invalidates even copies that completed before it.
	return mem::guest::write_virt(caller, slat, result_va, &out, sizeof(out));
}
