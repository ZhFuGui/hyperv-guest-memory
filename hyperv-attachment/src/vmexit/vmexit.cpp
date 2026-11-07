#include "vmexit.hpp"
#include "hypercall/hypercall.hpp"
#include "first/first.hpp"
#include "../vcpu/vcpu.hpp"
#include "../arch/current.hpp"
#include "../slat/violation/violation.hpp"
#include "../slat/hook/cache.hpp"
#include "../intr/nmi.hpp"
#include <boot_info.hpp>
#include <crt.hpp>

using handler_fn = std::uint64_t(*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);

namespace
{
	handler_fn orig_handler = nullptr;
	
	bool handle_vmexit(guest_regs& regs) noexcept
	{
		switch (arch::exit_reason())
		{
		case exit_reason::cpuid:
			if (!hc::handle(regs)) return false;
			arch::advance_rip();
			return true;
		case exit_reason::slat_violation:
			return slat::violation::handle();
		case exit_reason::nmi:
			intr::nmi::process();
			break;
		default:
			break;
		}

		return false;
	}
}

void vmexit::init(const boot_info& bi)
{
	orig_handler = reinterpret_cast<handler_fn>(bi.orig_vmexit_handler);
}

std::uint64_t vmexit::detour(const std::uint64_t a1, const std::uint64_t a2,
                              const std::uint64_t a3, const std::uint64_t a4)
{
	if (vcpu::curr_id() >= vcpu::count())
		return orig_handler(a1, a2, a3, a4);
	if (!first::handle()) return orig_handler(a1, a2, a3, a4);
	first::handle_non_bsp();

	guest_regs regs = arch::guest_regs(a1, a2);
	slat::cache::handle();
	if (arch::exit_reason() != exit_reason::slat_violation)
		vcpu::curr().retry_gpa = UINT64_MAX;

	if (handle_vmexit(regs))
	{
		arch::set_guest_regs(a1, a2, regs);
		return arch::handled_vmexit_result(a1);
	}
	return orig_handler(a1, a2, a3, a4);
}
