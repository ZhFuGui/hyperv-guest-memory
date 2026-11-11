#pragma once
#include <cstdint>

namespace hc
{
	inline constexpr std::uint64_t allowed_key = 0x8A3B55C7927;

	enum class type : std::uint64_t
	{
		test = 0,
		curr_cr3 = 1,
		phys_mem_op = 2,
		virt_mem_op = 3,
		translate_va = 4,
		hook_add = 5,
		hook_remove = 6,
		hide_page = 7,
		rem_heap_size = 8,
		add_log = 9,
		flush_logs = 10,
		// RDX=source CR3, R8=items VA, R9=count, R10=mask VA, R11=0.
		// RAX=1 iff all items were attempted and the mask was written.
		virt_read_batch = 11,
		// RDX=source CR3, R8=request VA, R9=result VA, R10=R11=0.
		// RAX=1 iff the full result was published; inspect result.status.
		virt_find_list = 12
	};

	union info
	{
		std::uint64_t val;

		struct
		{
			type type : 16;
			std::uint64_t key : 48;
		};
	};

	struct params
	{
		info info;
		std::uint64_t args[5];
	};
}
