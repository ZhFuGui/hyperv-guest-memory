#pragma once
#include <cstdint>
#include <identity_map.hpp>
namespace test { void* map_hpa(std::uintptr_t address) noexcept; }
namespace mem
{
	template <class T = void*>
	T map_phys(std::uintptr_t address) noexcept
	{
		if (address >= hva_identity_map_size)
			return T{};
		return reinterpret_cast<T>(test::map_hpa(address));
	}
}
