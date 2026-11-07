#include "cache.hpp"
#include "../../vcpu/vcpu.hpp"
#include "../../arch/current.hpp"
#include "../../intr/nmi.hpp"
#include <atomic>

namespace { constinit std::atomic<std::uint64_t> generation{1}; }

bool slat::cache::handle() noexcept
{
	auto& cpu = vcpu::curr();
	const auto target = generation.load(std::memory_order_acquire);
	auto observed = cpu.observed_epoch.load(std::memory_order_acquire);
	if (observed >= target)
		return false;
	arch::flush_slat_cache();
	// An NMI may have acknowledged a newer epoch while this call was interrupted.
	cpu.observed_epoch.compare_exchange_strong(observed, target, std::memory_order_release);
	return true;
}

std::uint64_t slat::cache::flush_all(const bool notify) noexcept
{
	const auto target = generation.fetch_add(1, std::memory_order_acq_rel) + 1;
	handle();
	if (notify) intr::nmi::send_all_but_self();
	return target;
}

std::uint64_t slat::cache::epoch() noexcept { return generation.load(std::memory_order_acquire); }

std::size_t slat::cache::pending(const std::uint64_t target) noexcept
{
	std::size_t count = 0;
	vcpu::foreach([&](vcpu& cpu, std::size_t) {
		if (cpu.observed_epoch.load(std::memory_order_acquire) < target)
			++count;
	});
	return count;
}
