#include "trap.hpp"
#include "../cr3/cr3.hpp"
#include "../cr3/split.hpp"
#include "../../arch/current.hpp"
#include "../../capture/capture.hpp"
#include "../../mem/mem.hpp"

#include <crt.hpp>
#include <mutex.hpp>

namespace
{
	constexpr std::size_t table_size = 8192;
	constexpr std::size_t table_mask = table_size - 1;

	slat::hook::trap_entry table[table_size];
	std::size_t used_count = 0;
	std::size_t captured_count = 0;
	cstd::mutex mtx;

	std::size_t slot_of(const phys_addr gpa) noexcept
	{
		return static_cast<std::size_t>((gpa >> page_shift) & table_mask);
	}

	slat::hook::trap_entry* find_locked(const phys_addr gpa) noexcept
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

	slat::hook::trap_entry* find_empty_locked(const phys_addr gpa) noexcept
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

	{
		cstd::lock_guard guard{ mtx };

		if (find_locked(page))
			return true;

		auto* const entry = find_empty_locked(page);
		if (!entry)
			return false;

		entry->gpa = page;
		entry->hits = 0;
		entry->captured = false;
		entry->used = true;
		used_count++;
	}

	auto* const pte = cr3::find_split_leaf(cr3::hyperv, page);
	if (!pte)
	{
		cstd::lock_guard guard{ mtx };
		if (auto* const entry = find_locked(page))
		{
			entry->used = false;
			used_count--;
		}
		return false;
	}

	arch::slat_set_perm(*pte, page_perm::read);
	arch::flush_slat_cache();

	return true;
}

bool slat::hook::trap_capture(const phys_addr gpa) noexcept
{
	const auto page = gpa & ~page_mask;

	{
		cstd::lock_guard guard{ mtx };

		auto* const entry = find_locked(page);
		if (!entry || entry->captured)
			return false;

		entry->captured = true;
		entry->hits++;
		captured_count++;
	}

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

	{
		cstd::lock_guard guard{ mtx };

		auto* const entry = find_locked(page);
		if (!entry)
			return false;

		entry->used = false;
		used_count--;
		if (entry->captured)
			captured_count--;
	}

	restore_permissions(page);

	return true;
}

void slat::hook::trap_clear_all() noexcept
{
	cstd::lock_guard guard{ mtx };

	for (std::size_t i = 0; i < table_size; i++)
	{
		if (!table[i].used)
			continue;

		auto* const pte = cr3::find_split_leaf(cr3::hyperv, table[i].gpa);
		if (pte)
			arch::slat_set_perm(*pte, page_perm::rwx);

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
	cstd::lock_guard guard{ mtx };
	return used_count;
}

std::size_t slat::hook::trap_captured() noexcept
{
	cstd::lock_guard guard{ mtx };
	return captured_count;
}
