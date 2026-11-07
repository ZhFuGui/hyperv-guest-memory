#pragma once
#include "../../arch/defs.hpp"
#include <list.hpp>
#include <mutex.hpp>
#include <atomic>

namespace slat::hook
{
	struct info
	{
		phys_addr gpa;
		phys_addr original_hpa;
		phys_addr shadow_hpa;
	};

	inline cstd::single_linked_list<info> list;
	inline cstd::shared_mutex mtx;
	inline constinit std::atomic<std::uint64_t> shadow_owner{UINT64_MAX};
}
