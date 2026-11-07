#include "mock.hpp"
#include "slat/hook/cache.hpp"
#include "slat/cr3/split.hpp"
#include "crt.hpp"
#include <array>
#include <cstring>
#include <new>
#include <vector>
#include <stdexcept>

namespace mock
{
	thread_local std::size_t cpu = 0;
	std::size_t cpu_count = 16;
	thread_local ::cr3 root{}, guest_root{};
	thread_local std::uint64_t rip = 0x900000;
	thread_local ::guest_regs regs{};
	thread_local enum exit_reason reason = exit_reason::unknown;
	thread_local ::slat_violation fault{};
	bool deliver_nmis = true, allocation_fails = false, missing_leaf = false, mbec = false;
	std::atomic<unsigned> allocations{}, notifications{}, original_calls{};
	std::function<void()> on_flush, on_mode_query, on_init;
	std::function<void(void*, const void*, std::size_t)> on_copy;
	::cr3 system_guest_root{};
	std::array<vcpu, 64> cpus;
	std::vector<void*> pages;
	// Keep guest output buffers outside the GPA ranges used by trap stress tests.
	std::uintptr_t next_guest_gpa = 0x7000000000ull;

	void* allocate_page()
	{
		auto* p = ::operator new(page_size, std::align_val_t{page_size});
		std::memset(p, 0, page_size);
		pages.push_back(p);
		++allocations;
		return p;
	}

	std::uint64_t* map_page(const ::cr3 directory, const std::uintptr_t address, const std::uintptr_t physical)
	{
		auto* table = reinterpret_cast<std::uint64_t*>(directory.address_of_page_directory << page_shift);
		for (unsigned shift : {39u, 30u, 21u})
		{
			auto& item = table[(address >> shift) & 511];
			if (!(item & 1))
			{
				auto* child = allocate_page();
				item = reinterpret_cast<std::uintptr_t>(child) | 7;
#if defined(MOCK_INTEL)
				item |= (1ull << 10);
#endif
				if (directory.flags == system_guest_root.flags)
					map_page(slat::cr3::hyperv, reinterpret_cast<std::uintptr_t>(child), reinterpret_cast<std::uintptr_t>(child));
			}
			table = reinterpret_cast<std::uint64_t*>(item & 0x000ffffffffff000ull);
		}
		auto* leaf = &table[(address >> page_shift) & 511];
		*leaf = (physical & ~page_mask) | 7;
#if defined(MOCK_INTEL)
		*leaf |= (1ull << 10);
#endif
		return leaf;
	}

	void setup()
	{
		root.flags = reinterpret_cast<std::uintptr_t>(allocate_page());
		slat::cr3::hyperv = root;
		system_guest_root.flags = reinterpret_cast<std::uintptr_t>(allocate_page());
		guest_root = system_guest_root;
		map_page(root, guest_root.flags, guest_root.flags);
	}

	void bind(const std::size_t id)
	{
		cpu = id; root = slat::cr3::hyperv; guest_root = system_guest_root;
	}

	real_arch::slat_pte* leaf(const phys_addr page) { return slat::cr3::find_leaf(slat::cr3::hyperv, page); }

	void map_guest_buffer(void* p, const std::size_t size)
	{
		const auto start = reinterpret_cast<std::uintptr_t>(p) & ~page_mask;
		const auto last = (reinterpret_cast<std::uintptr_t>(p) + size - 1) & ~page_mask;
		for (auto va = start; ; va += page_size)
		{
			const auto gpa = next_guest_gpa;
			next_guest_gpa += page_size;
			map_page(slat::cr3::hyperv, gpa, va);
			map_page(system_guest_root, va, gpa);
			if (va == last) break;
		}
	}

	std::uint64_t original_handler(std::uint64_t a1, std::uint64_t, std::uint64_t, std::uint64_t)
	{
		++original_calls;
		return a1 + 7;
	}
}

vcpu& vcpu::curr() { return mock::cpus[mock::cpu]; }
vcpu& vcpu::get(std::size_t id) { return mock::cpus[id]; }

void cstd::crt::memcpy(void* dst, const void* src, const std::size_t count)
{
	std::memcpy(dst, src, count);
	if (mock::on_copy) mock::on_copy(dst, src, count);
}
void cstd::crt::memset(void* dst, const std::int32_t value, const std::size_t count)
{
	if (count) std::memset(dst, value, count);
}

void intr::nmi::init() noexcept { if (mock::on_init) mock::on_init(); }
void intr::nmi::process() noexcept { slat::cache::handle(); }
void intr::nmi::send_all_but_self() noexcept
{
	++mock::notifications;
	if (!mock::deliver_nmis) return;
	const auto self = mock::cpu;
	for (std::size_t i = 0; i < mock::cpu_count; ++i)
	{
		if (i == self) continue;
		mock::cpu = i;
		slat::cache::handle();
	}
	mock::cpu = self;
}

bool hc::handle(::guest_regs& regs)
{
	if (regs.rcx != 1) return false;
	regs.rax = 0x1337;
	return true;
}
