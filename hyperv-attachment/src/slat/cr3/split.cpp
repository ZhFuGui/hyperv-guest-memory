#include "split.hpp"
#include "../../mem/map.hpp"
#include "../../mem/alloc/page.hpp"
#include <atomic>

namespace
{
	bool split_2mb(arch::slat_pde& pde_ref) noexcept
	{
		const auto large = reinterpret_cast<const arch::slat_pde_2mb&>(pde_ref);

		auto* pt = static_cast<arch::slat_pte*>(mem::page::try_alloc());
		if (!pt)
			return false;

		for (std::uint64_t i = 0; i < paging_entry_count; i++)
			arch::slat_init_split_pte(pt[i], large, i);

		arch::slat_set_table_pde(pde_ref, mem::pfn_for_map(pt));
		return true;
	}

	bool split_1gb(arch::slat_pdpte& pdpte_ref) noexcept
	{
		const auto large = reinterpret_cast<const arch::slat_pdpte_1gb&>(pdpte_ref);

		auto* pd = static_cast<arch::slat_pde_2mb*>(mem::page::try_alloc());
		if (!pd)
			return false;

		for (std::uint64_t i = 0; i < paging_entry_count; i++)
			arch::slat_init_split_pde(pd[i], large, i);

		arch::slat_set_table_pdpte(pdpte_ref, mem::pfn_for_map(pd));
		return true;
	}
}

namespace
{
arch::slat_pte* lookup_leaf(const ::cr3 slat_cr3, const phys_addr gpa,
                          const bool split, const bool require_execute,
                          const std::uint64_t execute_mask = 0) noexcept
{
	if (gpa >= (1ull << 48)) return nullptr; // this walker implements four levels
	const virt_addr va{ .val = gpa };
	auto allowed = execute_mask ? execute_mask : arch::slat_active_execute_mask();

	auto* pml4 = mem::map_phys<arch::slat_pml4e*>(
		slat_cr3.address_of_page_directory << page_shift);
	const arch::slat_pml4e pml4e{.flags = std::atomic_ref<std::uint64_t>(pml4[va.pml4e].flags).load()};

	if (!arch::slat_is_present(pml4e))
		return nullptr;
	allowed &= pml4e.flags ^ arch::slat_nx_bits;
	if (require_execute && !allowed)
		return nullptr;

	auto* pdpt = mem::map_phys<arch::slat_pdpte*>(
		pml4e.page_frame_number << page_shift);
	auto& live_pdpte = pdpt[va.pdpte];
	arch::slat_pdpte pdpte{.flags = std::atomic_ref<std::uint64_t>(live_pdpte.flags).load()};

	if (!arch::slat_is_present(pdpte))
		return nullptr;
	allowed &= pdpte.flags ^ arch::slat_nx_bits;
	if (require_execute && !allowed)
		return nullptr;

	if (arch::slat_is_large(pdpte))
	{
		if (!split || !split_1gb(live_pdpte))
			return nullptr;
		pdpte.flags = std::atomic_ref<std::uint64_t>(live_pdpte.flags).load();
	}

	auto* pd = mem::map_phys<arch::slat_pde*>(
		pdpte.page_frame_number << page_shift);
	auto& live_pde = pd[va.pde];
	arch::slat_pde pde{.flags = std::atomic_ref<std::uint64_t>(live_pde.flags).load()};

	if (!arch::slat_is_present(pde))
		return nullptr;
	allowed &= pde.flags ^ arch::slat_nx_bits;
	if (require_execute && !allowed)
		return nullptr;

	if (arch::slat_is_large(pde))
	{
		if (!split || !split_2mb(live_pde))
			return nullptr;
		pde.flags = std::atomic_ref<std::uint64_t>(live_pde.flags).load();
	}

	auto* pt = mem::map_phys<arch::slat_pte*>(
		pde.page_frame_number << page_shift);

	auto* leaf = &pt[va.pte];
	if (require_execute)
	{
		const arch::slat_pte snapshot{.flags = std::atomic_ref<std::uint64_t>(leaf->flags).load()};
		if (!arch::slat_is_present(snapshot) || !(allowed & (snapshot.flags ^ arch::slat_nx_bits))) return nullptr;
	}
	return leaf;
}
}

arch::slat_pte* slat::cr3::find_split_leaf(const ::cr3 root, const phys_addr gpa) noexcept
{
	return lookup_leaf(root, gpa, true, false);
}

arch::slat_pte* slat::cr3::find_leaf(const ::cr3 root, const phys_addr gpa) noexcept
{
	return lookup_leaf(root, gpa, false, false);
}

bool slat::cr3::allows_execute(const ::cr3 root, const phys_addr gpa, const std::uint64_t mask) noexcept
{
	return lookup_leaf(root, gpa, false, true, mask) != nullptr;
}

bool slat::cr3::merge(const ::cr3 slat_cr3, const phys_addr gpa) noexcept
{
	const virt_addr va{ .val = gpa };

	auto* pml4 = mem::map_phys<arch::slat_pml4e*>(
		slat_cr3.address_of_page_directory << page_shift);
	auto& pml4e = pml4[va.pml4e];

	if (!arch::slat_is_present(pml4e))
		return false;

	auto* pdpt = mem::map_phys<arch::slat_pdpte*>(
		pml4e.page_frame_number << page_shift);
	auto& pdpte = pdpt[va.pdpte];

	if (!arch::slat_is_present(pdpte) || arch::slat_is_large(pdpte))
		return false;

	auto* pd = mem::map_phys<arch::slat_pde*>(
		pdpte.page_frame_number << page_shift);
	auto& pde = pd[va.pde];

	if (!arch::slat_is_present(pde) || arch::slat_is_large(pde))
		return true;

	const auto pt_phys = pde.page_frame_number << page_shift;

	auto* pt = mem::map_phys<arch::slat_pte*>(pt_phys);

	auto& large = reinterpret_cast<arch::slat_pde_2mb&>(pde);
	arch::slat_init_merge_pde(large, pt[va.pte]);

	mem::page::free(mem::map_phys(pt_phys));
	return true;
}
