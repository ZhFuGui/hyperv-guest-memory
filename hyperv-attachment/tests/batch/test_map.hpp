#pragma once
#include <cstdint>
namespace test { void* map_hpa(std::uintptr_t address) noexcept; }
namespace mem
{
	template <class T = void*>
	T map_phys(std::uintptr_t address) noexcept
	{
		return reinterpret_cast<T>(test::map_hpa(address));
	}
}
