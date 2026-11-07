#pragma once
#include "../../arch/defs.hpp"
#include <capture.hpp>

namespace slat::hook
{
	bool trap_exec(phys_addr gpa, bool notify = true) noexcept;
	bool trap_capture(phys_addr gpa, std::uint64_t execute_mask = 0) noexcept;
	bool trap_remove(phys_addr gpa) noexcept;
	bool trap_clear_all() noexcept;
	std::size_t trap_count() noexcept;
	std::size_t trap_captured() noexcept;
	// Called with hook::mtx held by a control operation.
	bool trap_blocks_hooks() noexcept;
	void trap_note_unhandled(const slat_violation& fault) noexcept;
	capture::trap_status trap_stats() noexcept;
}
