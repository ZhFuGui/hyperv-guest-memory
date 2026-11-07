#include "hook.hpp"
#include "info.hpp"
#include "../cr3/cr3.hpp"
#include "../cr3/split.hpp"
#include "../../arch/current.hpp"
#include "../../mem/alloc/page.hpp"
#include "../../mem/map.hpp"
#include "cache.hpp"
#include "trap.hpp"
#include "../../vcpu/vcpu.hpp"

namespace
{
	constexpr phys_addr large_page_2mb_mask = (1ULL << large_page_shift) - 1;

	bool hook_in_merge_range(const phys_addr gpa) noexcept
	{
		const auto base = gpa & ~large_page_2mb_mask;

		for (const auto& entry : slat::hook::list)
		{
			if ((entry.gpa & ~large_page_2mb_mask) == base)
				return true;
		}

		return false;
	}

	// its own lock, since the hiding below takes hook::mtx for itself
	cstd::mutex shadow_mtx;
	bool shadow_hidden = false;

	bool ensure_shadow() noexcept
	{
		cstd::unique_lock guard(shadow_mtx, cstd::try_to_lock);
		if (!guard.owns_lock()) return false;

		if (slat::cr3::shadow.flags && shadow_hidden)
			return true;

		{
			cstd::unique_lock mutation(slat::hook::mtx, cstd::try_to_lock);
			if (!mutation.owns_lock() || slat::hook::trap_blocks_hooks()) return false;
			slat::hook::shadow_owner.store(vcpu::curr_id(), std::memory_order_release);
		}
		struct reset_owner
		{
			~reset_owner() { slat::hook::shadow_owner.store(UINT64_MAX, std::memory_order_release); }
		} reset;
		if (!slat::cr3::shadow.flags) slat::cr3::init_shadow();

		if (!slat::cr3::shadow.flags)
			return false;

		shadow_hidden = slat::hook::hide_heap();
		return shadow_hidden;
	}
}

void slat::hook::init(const boot_info& bi) noexcept
{
	init_hide(bi);
}

bool slat::hook::add(const phys_addr gpa, const phys_addr shadow_hpa) noexcept
{
	{
		cstd::unique_lock guard(hook::mtx, cstd::try_to_lock);
		if (!guard.owns_lock() || shadow_owner.load(std::memory_order_acquire) != UINT64_MAX || trap_blocks_hooks()) return false;
	}
	if (!ensure_shadow())
		return false;

	{
		cstd::unique_lock guard(hook::mtx, cstd::try_to_lock);
		if (!guard.owns_lock() || shadow_owner.load(std::memory_order_acquire) != UINT64_MAX || trap_blocks_hooks()) return false;

		auto* orig_pte = cr3::find_split_leaf(cr3::hyperv, gpa);
		if (!orig_pte)
			return false;

		auto* shadow_pte = cr3::find_split_leaf(cr3::shadow, gpa);
		if (!shadow_pte)
			return false;

		const auto original_hpa = orig_pte->page_frame_number << page_shift;

		arch::slat_set_perm(*orig_pte, page_perm::rw);

		shadow_pte->page_frame_number = shadow_hpa >> page_shift;
		arch::slat_set_perm(*shadow_pte, page_perm::rwx);

		list.push_front({ gpa, original_hpa, shadow_hpa });
	}

	cache::flush_all();
	return true;
}

bool slat::hook::remove(const phys_addr gpa) noexcept
{
	bool found = false;

	{
		cstd::unique_lock guard(hook::mtx, cstd::try_to_lock);
		if (!guard.owns_lock() || shadow_owner.load(std::memory_order_acquire) != UINT64_MAX || trap_blocks_hooks()) return false;

		list.remove_if([gpa, &found](const info& entry)
		{
			if (entry.gpa != gpa)
				return false;

			found = true;

			if (auto* orig_pte = cr3::find_split_leaf(cr3::hyperv, gpa))
				arch::slat_set_perm(*orig_pte, page_perm::rwx);

			if (auto* shadow_pte = cr3::find_split_leaf(cr3::shadow, gpa))
			{
				shadow_pte->page_frame_number = entry.original_hpa >> page_shift;
				arch::slat_set_perm(*shadow_pte, page_perm::rwx);
			}

			return true;
		});

		if (found && !hook_in_merge_range(gpa))
		{
			cr3::merge(cr3::hyperv, gpa);
			cr3::merge(cr3::shadow, gpa);
		}
	}

	if (found)
		cache::flush_all();

	return found;
}
