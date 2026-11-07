#include "first.hpp"
#include "../../arch/defs.hpp"
#include "../../mem/map.hpp"
#include "../../vcpu/vcpu.hpp"
#include "../../slat/slat.hpp"
#include "../../slat/cr3/cr3.hpp"
#include "../../slat/hook/hook.hpp"
#include "../../intr/nmi.hpp"

#include <atomic>
#include <boot_info.hpp>
#include <crt.hpp>

namespace
{
	phys_range boot_img;

	void wipe_boot_img() noexcept
	{
		const auto img_base = mem::map_phys<std::uint8_t*>(boot_img.base);

		cstd::crt::memset(img_base, 0, boot_img.size);
	}
}

void vmexit::first::init(const boot_info& bi)
{
	boot_img = bi.boot_img;
}

bool vmexit::first::handle()
{
	static std::atomic<unsigned> state{};
	if (state.load(std::memory_order_acquire) == 2) return true;
	unsigned expected = 0;
	if (!state.compare_exchange_strong(expected, 1)) return false;

	wipe_boot_img();
	slat::init();
	intr::nmi::init();
	state.store(2, std::memory_order_release);
	return true;
}

void vmexit::first::handle_non_bsp()
{
	static std::atomic<bool> handled{};

	if (handled.load(std::memory_order_acquire))
		return;

	if (vcpu::is_bsp())
		return;

	if (handled.exchange(true))
		return;

	// the shadow is taken by the first hook, which hides this for itself
	if (!slat::hook::hide_heap()) handled.store(false, std::memory_order_release);
}
