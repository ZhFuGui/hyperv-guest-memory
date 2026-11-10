#pragma once
#include <cstdint>
#include <cstddef>

inline constexpr std::size_t hva_identity_map_pml4e = 255;
// One PML4 slot, backed by the loader's 512 one-GiB PDPT leaves.
inline constexpr std::uintptr_t hva_identity_map_size = 1ULL << 39;

constexpr std::uintptr_t hva_identity_map_base()
{
	constexpr std::size_t pml4e_shift = 39;

	return hva_identity_map_pml4e << pml4e_shift;
}
