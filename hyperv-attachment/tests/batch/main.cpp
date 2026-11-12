#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>
#include <crt.hpp>
#include <boot_info.hpp>
#include "test_arch.hpp"
#include "../../src/vcpu/vcpu.hpp"
#include "../../src/vmexit/hypercall/batch.hpp"
#include "../../src/vmexit/hypercall/hypercall.hpp"
#include "../../src/mem/guest.hpp"
#include "../../src/mem/alloc/slab.hpp"
#include "../../../usermode/src/hypercall/hypercall.hpp"

namespace test
{
	constexpr std::size_t arena_size = 16 * 1024 * 1024;
	alignas(4096) std::array<std::uint8_t, arena_size> arena{};
	std::uint64_t next_hpa = 4096, next_gpa = 0x10000000;
	std::unordered_map<std::uint64_t, std::uint64_t> gpa_hpa;
	thread_local cr3 caller{};
	cr3 slat{};
	thread_local unsigned long cpu = 0;
	thread_local std::function<void(std::uint64_t)> on_map;
	std::atomic<unsigned> slab_allocs = 0, slab_frees = 0;
	bool slab_failure = false;
	unsigned checks = 0;

	void check(bool value, const char* message)
	{
		++checks;
		if (!value) throw std::runtime_error(message);
	}
	void* map_hpa(std::uintptr_t address) noexcept
	{
		if (on_map) { auto callback = std::move(on_map); on_map = {}; callback(address); }
		return address && address < arena.size() ? arena.data() + address : nullptr;
	}
	std::uint64_t alloc_hpa()
	{
		if (next_hpa + 4096 > arena.size()) throw std::runtime_error("physical arena exhausted");
		const auto result = next_hpa; next_hpa += 4096;
		std::memset(arena.data() + result, 0, 4096);
		return result;
	}
	std::uint64_t* table(std::uint64_t hpa) { return reinterpret_cast<std::uint64_t*>(arena.data() + hpa); }
	void slat_map(std::uint64_t gpa, std::uint64_t hpa)
	{
		auto node = slat.flags;
		for (auto shift : {39, 30, 21}) {
			auto& entry = table(node)[(gpa >> shift) & 511];
			if (!entry) entry = alloc_hpa() | 7;
			node = entry & ~4095ULL;
		}
		table(node)[(gpa >> 12) & 511] = hpa ? hpa | 7 : 0;
	}
	std::uint64_t guest_page(std::uint64_t hpa = 0)
	{
		const auto gpa = next_gpa; next_gpa += 4096;
		if (!hpa) hpa = alloc_hpa();
		gpa_hpa[gpa] = hpa; slat_map(gpa, hpa); return gpa;
	}
	cr3 space() { return cr3{.flags = guest_page()}; }
	std::uint64_t* guest_leaf(cr3 root, std::uint64_t va)
	{
		auto node = root.flags & ~4095ULL;
		for (auto shift : {39, 30, 21}) {
			auto& entry = table(gpa_hpa.at(node))[(va >> shift) & 511];
			if (!entry) entry = guest_page() | 7;
			node = entry & ~4095ULL;
		}
		return &table(gpa_hpa.at(node))[(va >> 12) & 511];
	}
	std::uint8_t* map(cr3 root, std::uint64_t va)
	{
		const auto hpa = alloc_hpa();
		*guest_leaf(root, va) = guest_page(hpa) | 7;
		return arena.data() + hpa;
	}
	void reset()
	{
		on_map = {}; next_hpa = 4096; next_gpa = 0x10000000; gpa_hpa.clear();
		slat.flags = alloc_hpa(); caller = space(); cpu = 0;
	}
	constexpr std::uint64_t request_va = 0x1000000000, mask_va = request_va + 8192;
	constexpr std::uint64_t source_va = 0x2000000000, output_va = 0x3000000000;
	struct fixture
	{
		cr3 source;
		hc::batch_read_item* items;
		std::uint64_t* mask;
		fixture()
		{
			reset(); source = space();
			items = reinterpret_cast<hc::batch_read_item*>(map(caller, request_va));
			mask = reinterpret_cast<std::uint64_t*>(map(caller, mask_va));
			*mask = 0xAAAAAAAAAAAAAAAAULL;
		}
		bool run(std::uint64_t count) { return hc::batch::read(source.flags, request_va, count, mask_va); }
	};

	void ordinary_and_limits()
	{
		fixture f;
		auto* src = map(f.source, source_va); auto* dst = map(caller, output_va);
		for (unsigned i = 0; i < 4096; ++i) src[i] = static_cast<std::uint8_t>(i * 13);
		f.items[0] = {source_va, output_va, 4096};
		const auto allocations = slab_allocs.load();
		check(f.run(1) && *f.mask == 1 && !std::memcmp(src, dst, 4096), "single full page");
		check(slab_allocs == allocations, "no allocation on read path");
		for (std::uint64_t i = 0; i < hc::batch_max_items; ++i)
			f.items[i] = {source_va + i, output_va + i, 1};
		check(f.run(64) && *f.mask == UINT64_MAX, "64th success bit");
		f.items[1].source_va = source_va + 4096;
		check(f.run(64) && *f.mask == (UINT64_MAX ^ 2), "one source failure does not cancel others");
		for (unsigned i = 0; i < 64; ++i) f.items[i].source_va = source_va + 4096;
		check(f.run(64) && *f.mask == 0, "all failures still publish valid mask");
		f.items[0] = {UINT64_MAX, UINT64_MAX, 0};
		check(f.run(1) && *f.mask == 1, "zero size does not touch addresses");
		map(f.source, UINT64_MAX - 4095)[4095] = 0xA7;
		f.items[0] = {UINT64_MAX, output_va, 1};
		check(f.run(1) && *f.mask == 1 && dst[0] == 0xA7, "last canonical byte without range wrap");
		check(!f.run(0) && !f.run(65) && !f.run(0x100000001ULL), "count full-width validation");
		check(!hc::batch::read(0, request_va, 1, mask_va), "zero CR3");
		check(!hc::batch::read(f.source.flags | (1ULL << 48), request_va, 1, mask_va), "CR3 bits beyond walker");
		f.items[0] = {source_va, output_va, 1};
		check(hc::batch::read(f.source.flags | 0x123, request_va, 1, mask_va), "CR3 PCID retained");

		// Both endpoints have independently allocated, noncontiguous physical pages.
		std::vector<std::uint8_t*> sources, outputs;
		sources.push_back(src); outputs.push_back(dst);
		for (unsigned i = 1; i < 16; ++i) {
			sources.push_back(map(f.source, source_va + i * 4096));
			outputs.push_back(map(caller, output_va + i * 4096));
		}
		for (unsigned i = 0; i < 16; ++i) std::memset(sources[i], static_cast<int>(i + 1), 4096);
		f.items[0] = {source_va, output_va, hc::batch_max_bytes};
		check(f.run(1) && *f.mask == 1, "64 KiB accepted");
		for (unsigned i = 0; i < 16; ++i) check(!std::memcmp(sources[i], outputs[i], 4096), "noncontiguous payload");
		f.items[0].size++;
		std::memset(dst, 0xCC, 4096);
		check(!f.run(1) && dst[0] == 0xCC, "byte cap before payload");
		f.items[0].size = UINT64_MAX;
		check(!f.run(1), "size overflow rejected");
		f.items[0] = {source_va, output_va, hc::batch_max_bytes};
		f.items[1] = {source_va, output_va + hc::batch_max_bytes, 1};
		check(!f.run(2) && dst[0] == 0xCC, "sum cap rejected before payload");
	}

	void validation_and_failure()
	{
		fixture f;
		auto* src = map(f.source, source_va); auto* dst = map(caller, output_va);
		std::memset(src, 0x71, 4096); std::memset(dst, 0xCC, 4096);
		f.items[0] = {source_va, output_va, 8};
		f.items[1] = {source_va, output_va + 4, 8};
		check(!f.run(2) && dst[0] == 0xCC, "overlap rejected before first copy");
		f.items[1] = {source_va, request_va + 3, 1}; check(!f.run(2), "descriptor overwrite rejected");
		f.items[1] = {source_va, mask_va, 1}; check(!f.run(2), "mask overwrite rejected");
		f.items[1] = {0x0000800000000000ULL, output_va + 8, 1}; check(!f.run(2), "noncanonical source");
		f.items[1] = {source_va, UINT64_MAX - 1, 4}; check(!f.run(2), "destination wraparound");
		f.items[1] = {source_va, 0x00007FFFFFFFFFFFULL, 2}; check(!f.run(2), "canonical hole crossing");
		check(!hc::batch::read(f.source.flags, request_va, 1, mask_va + 1), "unaligned mask");
		check(!hc::batch::read(f.source.flags, request_va, 1, request_va), "control overlap");
		check(!hc::batch::read(f.source.flags, request_va + 4096, 1, mask_va), "missing descriptors");
		const auto split_va = request_va + 4090;
		const hc::batch_read_item split{source_va, output_va, 8};
		std::memcpy(reinterpret_cast<std::uint8_t*>(f.items) + 4090, &split, 6);
		check(!hc::batch::read(f.source.flags, split_va, 1, mask_va) && dst[0] == 0xCC,
		      "partial control read performs no payload writes");
		f.items[0] = {source_va + 4090, output_va, 12};
		check(f.run(1) && *f.mask == 0 && dst[0] == 0x71, "partial source failure is not success");
		f.items[0] = {source_va, output_va + 4090, 12};
		check(f.run(1) && *f.mask == 0, "partial destination failure is not success");
		f.items[0] = {source_va, output_va, 8};
		check(!hc::batch::read(f.source.flags, request_va, 1, mask_va + 4096), "response write failure");
		check(!vcpu::curr().batch_work->busy.load(), "workspace released after failure");
		f.items[1] = {source_va, output_va + 4096, 8};
		check(f.run(2) && *f.mask == 1, "destination failure isolated from successful item");

		bool nested = true;
		on_map = [&](std::uint64_t) { nested = f.run(1); };
		check(f.run(1) && !nested, "reentry fails without waiting");
		auto* const saved = vcpu::curr().batch_work;
		vcpu::curr().batch_work = nullptr;
		check(!f.run(1), "allocation failure fails closed");
		vcpu::curr().batch_work = saved;
	}

	void split_controls_and_host_copy()
	{
		fixture f;
		auto* second = map(caller, request_va + 4096);
		auto* src = map(f.source, source_va); auto* dst = map(caller, output_va);
		std::memset(src, 0x35, 4096);
		const hc::batch_read_item item{source_va, output_va + 7, 3};
		const auto split_va = request_va + 4090;
		std::memcpy(reinterpret_cast<std::uint8_t*>(f.items) + 4090, &item, 6);
		std::memcpy(second, reinterpret_cast<const std::uint8_t*>(&item) + 6, sizeof(item) - 6);
		check(hc::batch::read(f.source.flags, split_va, 1, mask_va) && *f.mask == 1 && dst[7] == 0x35,
		      "descriptor straddles two nonidentity pages");
		std::array<std::uint8_t, 32> expected{}, observed{};
		for (unsigned i = 0; i < expected.size(); ++i) expected[i] = static_cast<std::uint8_t>(i + 2);
		check(mem::guest::write_virt(caller, request_va + 4080, expected.data(), expected.size()), "legacy host write");
		check(mem::guest::read_virt(caller, request_va + 4080, observed.data(), observed.size()) && observed == expected,
		      "legacy host read uses GPA to HPA");
		const auto leaf = *guest_leaf(caller, mask_va);
		const auto gpa = leaf & ~4095ULL;
		// Missing SLAT leaf, even when the guest PTE is present.
		slat_map(gpa, 0);
		check(!mem::guest::read_virt(caller, mask_va, observed.data(), 8), "missing HPA does not dereference GPA");
	}

	void dispatch_and_snapshot()
	{
		fixture f;
		map(f.source, source_va)[0] = 0x77; auto* dst = map(caller, output_va);
		f.items[0] = {source_va, output_va, 1};
		guest_regs regs{};
		regs.rcx = (hc::allowed_key << 16) | 11;
		regs.rdx = f.source.flags; regs.r8 = request_va; regs.r9 = 1; regs.r10 = mask_va;
		check(hc::handle(regs) && regs.rax == 1 && *f.mask == 1, "real dispatcher operation 11");
		regs.r11 = 1; regs.rax = 99;
		check(hc::handle(regs) && regs.rax == 0, "reserved argument consumed with failure");
		regs.rcx ^= 1ULL << 16;
		check(!hc::handle(regs), "foreign key passed through");
		regs = {}; regs.rcx = hc::allowed_key << 16;
		check(hc::handle(regs) && regs.rax == 0x1337, "old test operation unchanged");
		// Observe the first source-page-table access: controls have been snapshotted.
		const auto source_root_hpa = gpa_hpa.at(f.source.flags);
		bool changed = false;
		std::function<void(std::uint64_t)> callback;
		callback = [&](std::uint64_t hpa) {
			if (hpa == source_root_hpa) {
				f.items[0].destination_va = output_va + 4096;
				changed = true;
			} else on_map = callback;
		};
		on_map = callback;
		dst[0] = 0;
		check(f.run(1) && changed && *f.mask == 1 && dst[0] == 0x77, "request snapshot resists subsequent mutation");
		f.items[0] = {source_va, output_va, 1};
		const auto saved_slat = slat;
		callback = [&](std::uint64_t hpa) {
			if (hpa == source_root_hpa) slat.flags = 0;
			else on_map = callback;
		};
		on_map = callback;
		const bool complete = f.run(1);
		slat = saved_slat; on_map = {};
		check(complete && *f.mask == 1, "one SLAT context for snapshot, payload and publication");
	}

	void parallel_and_random()
	{
		fixture f;
		auto* src = map(f.source, source_va);
		for (unsigned i = 0; i < 4096; ++i) src[i] = static_cast<std::uint8_t>(i * 7);
		struct controls { std::uint64_t request, mask, output; hc::batch_read_item* items; std::uint64_t* result; std::uint8_t* bytes; };
		std::array<controls, 8> work{};
		for (unsigned i = 0; i < work.size(); ++i) {
			auto& w = work[i]; w.request = request_va + 0x100000 + i * 0x10000;
			w.mask = w.request + 4096; w.output = output_va + i * 0x10000;
			w.items = reinterpret_cast<hc::batch_read_item*>(map(caller, w.request));
			w.result = reinterpret_cast<std::uint64_t*>(map(caller, w.mask));
			w.bytes = map(caller, w.output);
			for (unsigned j = 0; j < 64; ++j) w.items[j] = {source_va + j, w.output + j, 1};
		}
		const auto root = caller;
		std::atomic<unsigned> failures = 0;
		std::vector<std::thread> threads;
		for (unsigned i = 0; i < work.size(); ++i) threads.emplace_back([&, i] {
			cpu = i; caller = root; auto& w = work[i];
			for (unsigned run = 0; run < 300; ++run) {
				if (!hc::batch::read(f.source.flags, w.request, 64, w.mask) || *w.result != UINT64_MAX ||
				    std::memcmp(src, w.bytes, 64)) ++failures;
			}
		});
		for (auto& thread : threads) thread.join();
		check(failures == 0, "8 CPUs x 300 batches x 64 items");
		// Randomized offsets/lengths, with unmapped sources interspersed.
		std::uint32_t rng = 0xBADC0DE;
		for (unsigned run = 0; run < 500; ++run) {
			auto& w = work[0]; std::uint64_t expected = 0;
			for (unsigned i = 0; i < 64; ++i) {
				rng = rng * 1664525 + 1013904223;
				const auto length = 1 + ((rng >> 8) & 31);
				const bool available = (rng & 7) != 0;
				w.items[i] = {source_va + (available ? (rng >> 16) % (4096 - 32) : 4096), w.output + i * 32, length};
				if (available) expected |= 1ULL << i;
			}
			check(hc::batch::read(f.source.flags, w.request, 64, w.mask) && *w.result == expected, "random success bitmap");
			for (unsigned i = 0; i < 64; ++i)
				if (expected & (1ULL << i)) check(!std::memcmp(src + (w.items[i].source_va - source_va), w.bytes + i * 32,
				                                           static_cast<std::size_t>(w.items[i].size)), "random payload");
		}
	}

	void review_guards()
	{
		fixture f;
		auto* src = map(f.source, source_va); auto* dst = map(caller, output_va);
		src[0] = 0x5B; dst[0] = 0xCC;
		f.items[0] = {source_va, output_va, 1};
		cpu = static_cast<unsigned long>(vcpu::count());
		check(!f.run(1) && dst[0] == 0xCC, "CPU index at count rejected before access");
		cpu = 0xFFFFFFFFUL;
		check(!f.run(1), "huge CPU index rejected"); cpu = 0;
		const auto caller_saved = caller, slat_saved = slat;
		caller.flags = 0; check(!f.run(1), "zero caller root rejected"); caller = caller_saved;
		slat.flags = 0; check(!f.run(1), "zero SLAT root rejected"); slat = slat_saved;
		check(!vcpu::curr().batch_work->busy.load(), "zero-root failure releases workspace");

		const auto source_root_hpa = gpa_hpa.at(f.source.flags);
		slat_map(f.source.flags, hva_identity_map_size);
		check(f.run(1) && *f.mask == 0 && dst[0] == 0xCC, "source table beyond identity coverage");
		slat_map(f.source.flags, source_root_hpa);
		const auto caller_root_hpa = gpa_hpa.at(caller.flags);
		slat_map(caller.flags, hva_identity_map_size);
		check(!f.run(1), "caller table beyond identity coverage");
		slat_map(caller.flags, caller_root_hpa);

		const auto source_gpa = *guest_leaf(f.source, source_va) & ~4095ULL;
		slat_map(source_gpa, hva_identity_map_size);
		check(f.run(1) && *f.mask == 0, "source leaf beyond identity coverage");
		slat_map(source_gpa, gpa_hpa.at(source_gpa));
		const auto output_gpa = *guest_leaf(caller, output_va) & ~4095ULL;
		slat_map(output_gpa, hva_identity_map_size);
		check(f.run(1) && *f.mask == 0, "destination leaf beyond identity coverage");
		slat_map(output_gpa, gpa_hpa.at(output_gpa));
		const auto mask_gpa = *guest_leaf(caller, mask_va) & ~4095ULL;
		slat_map(mask_gpa, hva_identity_map_size);
		check(!f.run(1), "publication beyond identity coverage");
		slat_map(mask_gpa, gpa_hpa.at(mask_gpa));

		auto& slat_pml4e = table(slat.flags)[0]; const auto saved = slat_pml4e;
		slat_pml4e = hva_identity_map_size | 7;
		check(!f.run(1), "SLAT child table beyond identity coverage"); slat_pml4e = saved;
		check(f.run(1) && *f.mask == 1 && dst[0] == 0x5B, "valid call recovers after every rejected walk");
		check(!mem::copy_phys(hva_identity_map_size - 1, 4096, 2), "physical source crossing coverage");
		check(!mem::copy_phys(4096, hva_identity_map_size - 1, 2), "physical destination crossing coverage");
		check(!mem::copy_phys(UINT64_MAX, 4096, 1), "physical address wrap rejected");
		check(mem::copy_phys(UINT64_MAX, UINT64_MAX, 0), "empty physical copy remains no-op");
	}

	void page_walk_shapes()
	{
		reset();
		const cr3 root{.flags = alloc_hpa()};
		const auto pdpt = alloc_hpa(), pd = alloc_hpa(), pt = alloc_hpa(), data = alloc_hpa();
		const auto va = source_va + 0x1234;
		auto& pml4e = table(root.flags)[(va >> 39) & 511]; pml4e = pdpt | 7;
		auto& pdpte = table(pdpt)[(va >> 30) & 511]; pdpte = 0x40000000ULL | 0x87;
		check(mem::translate_va(root, va) == 0x40001234ULL, "one-GiB leaf snapshot");
		pdpte = pd | 7;
		auto& pde = table(pd)[(va >> 21) & 511]; pde = 0x400000ULL | 0x87;
		check(mem::translate_va(root, va) == 0x401234ULL, "two-MiB leaf snapshot");
		pde = pt | 7;
		auto& pte = table(pt)[(va >> 12) & 511]; pte = data | 7;
		check(mem::translate_va(root, va) == data + 0x234, "four-KiB leaf snapshot");
		for (auto* entry : {&pml4e, &pdpte, &pde, &pte}) {
			const auto saved = *entry; *entry &= ~1ULL;
			check(mem::translate_va(root, va) == 0, "absent entry at each level"); *entry = saved;
		}
		for (auto* entry : {&pml4e, &pdpte, &pde}) {
			const auto saved = *entry; *entry = hva_identity_map_size | 7;
			check(mem::translate_va(root, va) == 0, "out-of-coverage table at each level"); *entry = saved;
		}
		pte = hva_identity_map_size | 7;
		check(!mem::guest::map_gpa(root, va), "out-of-coverage final HPA rejected");
		check(mem::translate_va(cr3{.flags = hva_identity_map_size}, va) == 0, "out-of-coverage root rejected");
	}

	std::uint64_t sdk_result = 1, sdk_mask = 0;
	unsigned sdk_calls = 0;
	hc::params sdk_params{};
	void sdk()
	{
		std::array<hc::batch_read_item, 64> items{}; std::uint64_t mask = UINT64_MAX;
		sdk_calls = 0; sdk_result = 1; sdk_mask = 1ULL << 63;
		check(hc::read_virtual_batch(0x12345000, items.data(), 64, mask) && mask == sdk_mask && sdk_calls == 1, "SDK one invocation");
		check(sdk_params.info.type == hc::type::virt_read_batch && sdk_params.info.key == hc::allowed_key &&
		      sdk_params.args[0] == 0x12345000 && sdk_params.args[1] == reinterpret_cast<std::uintptr_t>(items.data()) &&
		      sdk_params.args[2] == 64 && sdk_params.args[4] == 0, "SDK ABI arguments");
		sdk_result = 2; check(!hc::read_virtual_batch(1, items.data(), 1, mask) && mask == 0, "SDK accepts only exactly 1");
		sdk_result = 0; check(!hc::read_virtual_batch(1, items.data(), 1, mask) && mask == 0, "SDK clears failed publication");
		const auto calls = sdk_calls;
		check(!hc::read_virtual_batch(1, nullptr, 1, mask) && !hc::read_virtual_batch(1, items.data(), 65, mask) &&
		      !hc::read_virtual_batch(1, items.data(), 0, mask) && sdk_calls == calls, "SDK local invalid input");
	}
}

unsigned long batch_test_cpu_id() noexcept { return test::cpu; }
namespace cstd::crt
{
	void* malloc(std::size_t size) { return std::malloc(size); }
	void free(void* buffer) { std::free(buffer); }
	void memcpy(void* dst, const void* src, std::size_t size) { std::memcpy(dst, src, size); }
	void memset(void* dst, std::int32_t value, std::size_t size) { std::memset(dst, value, size); }
}
namespace mem::slab
{
	void* alloc(std::size_t size) noexcept { ++test::slab_allocs; return test::slab_failure ? nullptr : std::calloc(1, size); }
	void free(void* buffer) noexcept { if (buffer) ++test::slab_frees; std::free(buffer); }
}
namespace mem::page { std::size_t free_count() noexcept { return 123; } }
namespace slat::hook
{
	bool add(phys_addr, phys_addr) noexcept { return false; }
	bool remove(phys_addr) noexcept { return false; }
	bool hide(phys_addr, phys_addr) noexcept { return false; }
}
namespace logs
{
	void add(const guest_regs&) noexcept {}
	std::size_t flush(cr3, std::uintptr_t, std::uint16_t) noexcept { return 0; }
}
extern "C" std::uint64_t send_hypercall(const hc::params* params)
{
	++test::sdk_calls; test::sdk_params = *params;
	if (params->info.type == hc::type::virt_find_list) {
		auto* result = reinterpret_cast<hc::list_find_result*>(params->args[2]);
		std::memset(result, 0xA5, sizeof(*result)); result->status = hc::list_find_status::more;
		return test::sdk_result;
	}
	if (params->info.type == hc::type::virt_read_dependent_batch) {
		auto* result = reinterpret_cast<hc::dependent_read_result*>(params->args[3]);
		std::memset(result, 0xA5, sizeof(*result)); result->status[0] = hc::dependent_read_status::success;
		return test::sdk_result;
	}
	*reinterpret_cast<std::uint64_t*>(params->args[3]) = test::sdk_mask;
	return test::sdk_result;
}

#include "list_tests.hpp"
#include "dependent_tests.hpp"

int main()
{
	try {
		boot_info bi{}; bi.cpu_count = 8; vcpu::init(bi);
		test::check(vcpu::count() == 8 && vcpu::curr().batch_work, "production vCPU workspace init");
		test::ordinary_and_limits(); test::validation_and_failure(); test::split_controls_and_host_copy();
		test::dispatch_and_snapshot(); test::parallel_and_random(); test::review_guards();
		test::page_walk_shapes(); test::sdk();
		test::list_basics(); test::list_failures(); test::list_mutation_and_dispatch(); test::list_sdk();
		test::list_split_and_object_bounds(); test::list_parallel();
		test::dependent_basics(); test::dependent_validation(); test::dependent_failures();
		test::dependent_snapshot_and_dispatch(); test::dependent_split(); test::dependent_random(); test::dependent_sdk();
		test::dependent_output_guards();
		// Reallocation moves ownership rather than losing or duplicating scratch.
		auto* saved = vcpu::get(0).batch_work;
		bi.cpu_count = 16; vcpu::init(bi);
		test::check(vcpu::get(0).batch_work == saved && vcpu::get(15).batch_work, "workspace survives vector growth");
		test::slab_failure = true; bi.cpu_count = 17; vcpu::init(bi);
		test::check(vcpu::get(16).batch_work == nullptr, "failed startup allocation leaves disabled CPU");
		test::slab_failure = false;
		{
			vcpu first, second;
			first.batch_work = new(mem::slab::alloc(sizeof(hc::batch::workspace))) hc::batch::workspace{};
			second.batch_work = new(mem::slab::alloc(sizeof(hc::batch::workspace))) hc::batch::workspace{};
			auto* const owned = first.batch_work;
			const auto frees = test::slab_frees.load();
			second = std::move(first);
			test::check(first.batch_work == nullptr && second.batch_work == owned && test::slab_frees == frees + 1,
			            "move assignment releases old workspace and transfers ownership");
			auto& same = second;
			second = std::move(same);
			test::check(second.batch_work == owned, "self move retains workspace");
		}
		std::cout << "ALL PASS, assertions=" << test::checks << ", parallel_batches=2400, random_batches=500\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "FAIL after " << test::checks << ": " << error.what() << '\n'; return 1;
	}
}
