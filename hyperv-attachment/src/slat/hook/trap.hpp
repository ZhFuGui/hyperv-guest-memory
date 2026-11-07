#pragma once
#include "../../arch/defs.hpp"

namespace slat::hook
{
	struct trap_entry
	{
		phys_addr gpa;
		std::uint32_t hits;
		bool captured;
		bool used;
	};

	bool trap_exec(phys_addr gpa) noexcept;
	bool trap_capture(phys_addr gpa) noexcept;
	bool trap_remove(phys_addr gpa) noexcept;
	void trap_release(phys_addr gpa) noexcept;
	void trap_clear_all() noexcept;
	std::size_t trap_count() noexcept;
	std::size_t trap_captured() noexcept;
}
