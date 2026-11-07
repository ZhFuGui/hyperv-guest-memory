#pragma once
#include "../../arch/current.hpp"

namespace slat::cr3
{
	arch::slat_pte* find_split_leaf(::cr3 slat_cr3, phys_addr gpa) noexcept;
	// Lookup only: never split, allocate or acquire an allocator lock on a fault.
	arch::slat_pte* find_leaf(::cr3 slat_cr3, phys_addr gpa) noexcept;
	bool allows_execute(::cr3 slat_cr3, phys_addr gpa, std::uint64_t execute_mask = 0) noexcept;

	bool merge(::cr3 slat_cr3, phys_addr gpa) noexcept;
}
