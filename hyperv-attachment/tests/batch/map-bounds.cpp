#include "../../src/mem/map.hpp"
#include <iostream>

int main()
{
	// Compile the real identity mapper without the physical-memory test alias.
	// These checks calculate pointers only; no identity address is dereferenced.
	constexpr std::uintptr_t limit = 1ULL << 39;
	if (mem::map_phys(limit) || mem::map_phys(UINT64_MAX))
	{
		std::cerr << "FAIL: production mapper accepts an HPA outside its 512 GiB slot\n";
		return 1;
	}
	if (reinterpret_cast<std::uintptr_t>(mem::map_phys(0)) != hva_identity_map_base() ||
	    reinterpret_cast<std::uintptr_t>(mem::map_phys(limit - 1)) != hva_identity_map_base() + limit - 1)
		return 2;
	std::cout << "MAP BOUNDS PASS (real mapper, no identity dereferences)\n";
	return 0;
}
