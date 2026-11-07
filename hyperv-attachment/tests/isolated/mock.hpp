#pragma once
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <forward_list>
#include <mutex>
#include <shared_mutex>
#include <guest_regs.hpp>
#include <boot_info.hpp>
#include <capture.hpp>
#if defined(MOCK_INTEL)
#include "arch/intel.hpp"
using real_arch = intel;
#else
#include "arch/amd.hpp"
using real_arch = amd;
#endif

namespace mock
{
	extern thread_local std::size_t cpu;
	extern std::size_t cpu_count;
	extern thread_local ::cr3 root, guest_root;
	extern thread_local std::uint64_t rip;
	extern thread_local ::guest_regs regs;
	extern thread_local enum exit_reason reason;
	extern thread_local ::slat_violation fault;
	extern bool deliver_nmis, allocation_fails, missing_leaf, mbec;
	extern std::atomic<unsigned> allocations, notifications, original_calls;
	extern std::function<void()> on_flush, on_mode_query, on_init;
	extern std::function<void(void*, const void*, std::size_t)> on_copy;
	void* allocate_page();
	void setup();
	std::uint64_t* map_page(::cr3 root, std::uintptr_t address, std::uintptr_t physical);
	real_arch::slat_pte* leaf(phys_addr page);
	void bind(std::size_t);
	void map_guest_buffer(void*, std::size_t);
	std::uint64_t original_handler(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
}

struct arch : real_arch
{
	static std::uint64_t slat_active_execute_mask()
	{
		if (mock::on_mode_query) { auto cb = std::move(mock::on_mode_query); mock::on_mode_query = nullptr; cb(); }
#if defined(MOCK_INTEL)
		return mock::mbec ? slat_execute_mask : (1ull << 2);
#else
		return slat_execute_mask;
#endif
	}
	static std::uint64_t slat_fault_execute_mask(std::uint64_t qualification)
	{
#if defined(MOCK_INTEL)
		return mock::mbec && (qualification & (1ull << 9)) ? (1ull << 10) : (1ull << 2);
#else
		(void)qualification; return slat_execute_mask;
#endif
	}
	static ::cr3 slat_cr3() { return mock::root; }
	static void set_slat_cr3(::cr3 root) { mock::root = root; }
	static ::cr3 guest_cr3() { return mock::guest_root; }
	static std::uint64_t rip() { return mock::rip; }
	static void set_rip(std::uint64_t value) { mock::rip = value; }
	static void advance_rip() { mock::rip += 2; }
	static ::guest_regs guest_regs(std::uint64_t, std::uint64_t) { return mock::regs; }
	static void set_guest_regs(std::uint64_t, std::uint64_t, const ::guest_regs& regs) { mock::regs = regs; }
	static enum exit_reason exit_reason() { return mock::reason; }
	static ::slat_violation slat_violation() { return mock::fault; }
	static void flush_slat_cache()
	{
		if (mock::on_flush) { auto cb = std::move(mock::on_flush); mock::on_flush = nullptr; cb(); }
	}
};

namespace cstd
{
	using std::unique_lock; using std::shared_lock;
	using std::try_to_lock;
}

namespace mem
{
	template<class T = void*> T map_phys(std::uintptr_t address) noexcept
	{
		return mock::missing_leaf ? T{} : reinterpret_cast<T>(address);
	}
	inline std::uintptr_t phys_for_map(const void* p) noexcept { return reinterpret_cast<std::uintptr_t>(p); }
	inline std::uint64_t pfn_for_map(const void* p) noexcept { return phys_for_map(p) >> page_shift; }
}
namespace mem::page
{
	inline void* try_alloc(std::size_t = 1) noexcept { return mock::allocation_fails ? nullptr : mock::allocate_page(); }
	inline void* alloc(std::size_t = 1) noexcept { return try_alloc(); }
	inline void free(void*, std::size_t = 1) noexcept {}
}

struct vcpu
{
	std::atomic<std::uint64_t> observed_epoch{};
	std::uint64_t retry_gpa = UINT64_MAX, retry_rip = 0, retry_root = 0;
	static std::size_t curr_id() { return mock::cpu; }
	static std::size_t count() { return mock::cpu_count; }
	static bool is_bsp() { return curr_id() == 0; }
	static vcpu& curr();
	static vcpu& get(std::size_t);
	template<class F> static void foreach(F&& f) { for (std::size_t i=0; i<count(); ++i) f(get(i), i); }
};

namespace slat::cr3
{
	inline ::cr3 hyperv{}, shadow{};
	inline void init() { hyperv = mock::root; }
}
namespace slat::hook
{
	struct info { phys_addr gpa, original_hpa, shadow_hpa; };
	inline std::forward_list<info> list;
	inline std::shared_mutex mtx;
	inline std::atomic<std::uint64_t> shadow_owner{UINT64_MAX};
	inline void handle_page_split(phys_addr) {}
	inline bool hide_heap() { return true; }
}
namespace slat { inline void init() { cr3::init(); } }
namespace intr::nmi { void init() noexcept; void process() noexcept; void send_all_but_self() noexcept; }
namespace hc { bool handle(::guest_regs&); }
