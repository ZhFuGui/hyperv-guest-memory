#include "trap.hpp"
#include "../cr3/cr3.hpp"
#include "../cr3/split.hpp"
#include "../../arch/current.hpp"
#include "../../capture/capture.hpp"
#include "../../mem/mem.hpp"

#include <crt.hpp>
#include <intrin.h>

// Lock-free by design: this code runs inside VM-exit handlers, where a spinlock
// held by a vCPU that the outer hypervisor preempts would stall every other
// vCPU (observed as DPC_WATCHDOG_VIOLATION / bugcheck 0x133).
namespace
{
	constexpr std::size_t table_size = 8192;
	constexpr std::size_t table_mask = table_size - 1;

	slat::hook::trap_entry table[table_size];

	// statistics are advisory; races only make the counters approximate
	volatile std::uint32_t used_count = 0;
	volatile std::uint32_t captured_count = 0;

	std::size_t slot_of(const phys_addr gpa) noexcept
	{
		return static_cast<std::size_t>((gpa >> page_shift) & table_mask);
	}

	slat::hook::trap_entry* find(const phys_addr gpa) noexcept
	{
		for (std::size_t probe = 0; probe < table_size; probe++)
		{
			auto& entry = table[(slot_of(gpa) + probe) & table_mask];

			if (!entry.used)
				return nullptr;

			if (entry.gpa == gpa)
				return &entry;
		}

		return nullptr;
	}

	slat::hook::trap_entry* find_empty(const phys_addr gpa) noexcept
	{
		for (std::size_t probe = 0; probe < table_size; probe++)
		{
			auto& entry = table[(slot_of(gpa) + probe) & table_mask];

			if (!entry.used)
				return &entry;
		}

		return nullptr;
	}

	void restore_permissions(const phys_addr gpa) noexcept
	{
		auto* const pte = slat::cr3::find_split_leaf(slat::cr3::hyperv, gpa);
		if (!pte)
			return;

		arch::slat_set_perm(*pte, page_perm::rwx);
		arch::flush_slat_cache();
	}
}

bool slat::hook::trap_exec(const phys_addr gpa) noexcept
{
	const auto page = gpa & ~page_mask;

	if (find(page))
		return true;

	auto* const entry = find_empty(page);
	if (!entry)
		return false;

	auto* const pte = cr3::find_split_leaf(cr3::hyperv, page);
	if (!pte)
		return false;

	// Permissions first: if a fault races ahead of the table insert, the
	// violation handler's safety net cleans it up. The reverse order could
	// leave a page read-only with nobody owning the resulting fault.
	arch::slat_set_perm(*pte, page_perm::read);
	arch::flush_slat_cache();

	entry->gpa = page;
	entry->hits = 0;
	entry->captured = false;

	_ReadWriteBarrier();
	entry->used = true;
	used_count = used_count + 1;

	return true;
}

bool slat::hook::trap_capture(const phys_addr gpa) noexcept
{
	const auto page = gpa & ~page_mask;

	auto* const entry = find(page);
	if (!entry)
		return false;

	if (entry->captured)
	{
		// A concurrent scan re-armed a page that was already captured.
		// Restore execute permission and swallow the exit: returning false
		// leaves the violation unhandled and Hyper-V bugchecks 0x20001.
		restore_permissions(page);
		return true;
	}

	entry->captured = true;
	entry->hits = entry->hits + 1;
	captured_count = captured_count + 1;

	restore_permissions(page);

	const auto hpa = mem::translate_va(cr3::hyperv, page);
	const auto* const src = hpa
		? static_cast<const std::uint8_t*>(mem::map_phys(hpa))
		: nullptr;

	const auto rip = arch::rip();

	capture::push(arch::guest_cr3().flags,
	              rip & ~static_cast<std::uint64_t>(page_mask),
	              page, rip,
	              static_cast<std::uint32_t>(capture::reason::exec),
	              src);

	return true;
}

bool slat::hook::trap_remove(const phys_addr gpa) noexcept
{
	const auto page = gpa & ~page_mask;

	auto* const entry = find(page);
	if (!entry)
		return false;

	if (entry->captured)
		captured_count = captured_count - 1;

	entry->used = false;
	used_count = used_count - 1;

	restore_permissions(page);

	return true;
}

void slat::hook::trap_release(const phys_addr gpa) noexcept
{
	restore_permissions(gpa & ~page_mask);
}

void slat::hook::trap_clear_all() noexcept
{
	for (std::size_t i = 0; i < table_size; i++)
	{
		if (!table[i].used)
			continue;

		restore_permissions(table[i].gpa);

		table[i].used = false;
		table[i].captured = false;
		table[i].hits = 0;
	}

	arch::flush_slat_cache();
	used_count = 0;
	captured_count = 0;
}

std::size_t slat::hook::trap_count() noexcept
{
	return used_count;
}

std::size_t slat::hook::trap_captured() noexcept
{
	return captured_count;
}
