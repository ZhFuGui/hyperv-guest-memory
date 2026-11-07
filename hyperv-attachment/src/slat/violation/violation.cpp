#include "violation.hpp"
#include "../cr3/cr3.hpp"
#include "../cr3/split.hpp"
#include "../hook/info.hpp"
#include "../hook/page_split.hpp"
#include "../hook/trap.hpp"
#include "../../arch/current.hpp"
#include "../../vcpu/vcpu.hpp"

bool slat::violation::handle() noexcept
{
	const auto violation = arch::slat_violation();

	if (!violation.caused_by_translation)
		return false;

	if ((violation.access & page_perm::execute) == page_perm::none)
		return false;

	const auto page_gpa = violation.gpa & ~page_mask;

	const auto root = arch::slat_cr3();
	if (root.flags != cr3::hyperv.flags)
	{
		if (!cr3::shadow.flags || root.flags != cr3::shadow.flags)
			return false;
		cstd::shared_lock guard(hook::mtx, cstd::try_to_lock);
		if (!guard.owns_lock()) { hook::trap_note_unhandled(violation); return false; }
		hook::handle_page_split(page_gpa);

		arch::set_slat_cr3(cr3::hyperv);
		arch::flush_slat_cache();
		return true;
	}

	const auto execute_mask = arch::slat_fault_execute_mask(violation.qualification);
	if (slat::hook::trap_capture(page_gpa, execute_mask))
		return true;

	cstd::shared_lock guard(hook::mtx, cstd::try_to_lock);
	if (!guard.owns_lock()) { hook::trap_note_unhandled(violation); return false; }

	for (const auto& hook : hook::list)
	{
		if ((hook.gpa & ~page_mask) != page_gpa)
			continue;

		arch::set_slat_cr3(cr3::shadow);
		arch::flush_slat_cache();
		return true;
	}

	// A retired trap may leave an already-queued exit after its metadata was
	// reclaimed. Retry once only if the current page tables already allow X.
	// This invalidates stale translations; it never grants new permissions.
	auto& cpu = vcpu::curr();
	const auto rip = arch::rip();
	if (cr3::allows_execute(root, page_gpa, execute_mask) &&
		(cpu.retry_gpa != page_gpa || cpu.retry_rip != rip || cpu.retry_root != root.flags))
	{
		cpu.retry_gpa = page_gpa;
		cpu.retry_rip = rip;
		cpu.retry_root = root.flags;
		arch::flush_slat_cache();
		return true;
	}
	hook::trap_note_unhandled(violation);
	return false;
}
