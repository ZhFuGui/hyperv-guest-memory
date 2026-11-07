#include "mock.hpp"
#include "capture/capture.hpp"
#include "slat/hook/trap.hpp"
#include "slat/hook/cache.hpp"
#include "slat/cr3/split.hpp"
#include "slat/violation/violation.hpp"
#include "vmexit/vmexit.hpp"
#include "mem/guest.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
	unsigned checks = 0;
#if defined(MOCK_INTEL)
	constexpr std::uint64_t handled_result = 0;
#else
	constexpr std::uint64_t handled_result = 0x100;
#endif
	void check(const bool ok, const char* message)
	{
		++checks;
		if (!ok) throw std::runtime_error(message);
	}
	std::vector<capture::rec> buffer(capture::max_flush_records);
	std::array<std::uint8_t, page_size> bytes{};
	void* source_page = nullptr;

	bool push(const std::uint64_t value = 1)
	{
		return capture::push(0x4000, value, value, value, 0, bytes.data());
	}
	std::size_t flush(std::uint16_t count = capture::max_flush_records)
	{
		return capture::flush(mock::guest_root, reinterpret_cast<std::uintptr_t>(buffer.data()), count);
	}
	std::vector<capture::rec> drain()
	{
		std::vector<capture::rec> result;
		while (const auto n = flush()) result.insert(result.end(), buffer.begin(), buffer.begin() + n);
		return result;
	}
	arch::slat_pte* prepare(const phys_addr page)
	{
		mock::map_page(slat::cr3::hyperv, page, reinterpret_cast<std::uintptr_t>(source_page));
		return mock::leaf(page);
	}
	std::uint64_t flags(arch::slat_pte* p) { return std::atomic_ref<std::uint64_t>(p->flags).load(); }
	void flags(arch::slat_pte* p, std::uint64_t v) { std::atomic_ref<std::uint64_t>(p->flags).store(v); }
	bool nx(arch::slat_pte* p) { return (flags(p) & arch::slat_active_execute_mask()) == arch::slat_nx_bits; }
	void clean()
	{
		mock::bind(0);
		mock::on_copy = nullptr; mock::on_mode_query = nullptr; mock::on_flush = nullptr;
		mock::deliver_nmis = true; mock::allocation_fails = false;
		slat::hook::trap_clear_all();
		check(slat::hook::trap_count() == 0, "trap cleanup failed");
		check(capture::reset(), "ring reset busy without consumer");
	}

	void initialization_and_dispatch()
	{
		boot_info bi{};
		bi.orig_vmexit_handler = reinterpret_cast<std::uint8_t*>(&mock::original_handler);
		bi.boot_img.base = reinterpret_cast<std::uintptr_t>(mock::allocate_page());
		bi.boot_img.size = page_size;
		std::memset(reinterpret_cast<void*>(bi.boot_img.base), 0x5a, page_size);
		vmexit::init(bi);
		bool forwarded_during_init = false;
		mock::on_init = [&] {
			mock::bind(1);
			forwarded_during_init = vmexit::detour(0x100, 0, 0, 0) == 0x107;
			mock::bind(0);
		};
		mock::reason = exit_reason::unknown;
		vmexit::detour(0x100, 0, 0, 0);
		mock::on_init = nullptr;
		check(forwarded_during_init, "partially initialized state became visible");
		check(*reinterpret_cast<std::uint64_t*>(bi.boot_img.base) == 0x5a5a5a5a5a5a5a5aull, "capture fix unexpectedly enabled boot-image erasure");
		const auto calls = mock::original_calls.load();
		mock::reason = exit_reason::cpuid; mock::regs.rcx = 1; mock::rip = 0x900000;
		check(vmexit::detour(0x100, 0, 0, 0) == handled_result, "handled exit return ABI");
		check(mock::regs.rax == 0x1337 && mock::rip == 0x900002, "custom CPUID response or RIP advance");
		check(mock::original_calls == calls, "custom CPUID reached original handler");
		mock::regs.rcx = 0;
		check(vmexit::detour(0x100, 0, 0, 0) == 0x107 && mock::original_calls == calls + 1, "native CPUID not forwarded");
		mock::reason = exit_reason::nmi;
		vmexit::detour(0x100, 0, 0, 0);
		check(mock::original_calls == calls + 2, "genuine NMI was swallowed");
		std::cout << "PASS initialization publication and exit dispatch ABI\n";
	}

#if !defined(MOCK_INTEL)
	void amd_tlb_control_lifecycle()
	{
		clean(); mock::bind(0);
		auto& local = mock::vmcb_controls[0];
		local.tlb_control = tlb_control::flush_entire_tlb;
		mock::reason = exit_reason::cpuid; mock::regs.rcx = 1;
		check(vmexit::detour(0x100, 0, 0, 0) == handled_result, "AMD one-shot setup dispatch");
		check(local.tlb_control == tlb_control::do_not_flush, "consumed AMD TLB_CONTROL remained sticky");

		mock::deliver_nmis = false;
		auto& remote = mock::vmcb_controls[1];
		const auto calls = mock::original_calls.load();
		bool replayed = true;
		constexpr unsigned replay_rounds = 4096;
		for (unsigned round = 0; round < replay_rounds; ++round)
		{
			mock::bind(0);
			slat::cache::flush_all(false);
			mock::bind(1);
			mock::reason = exit_reason::unknown;
			replayed &= vmexit::detour(0x100, 0, 0, 0) == 0x107;
			replayed &= remote.tlb_control == tlb_control::flush_entire_tlb;
		}
		check(replayed, "Hyper-V cleared a pending AMD invalidation before VMRUN");
		check(mock::original_calls == calls + replay_rounds, "forwarded AMD exit skipped Hyper-V");
		check(vcpu::curr().observed_epoch.load() == slat::cache::epoch(),
			"replayed AMD invalidation was not acknowledged");

		mock::reason = exit_reason::cpuid; mock::regs.rcx = 1;
		check(vmexit::detour(0x100, 0, 0, 0) == handled_result, "AMD one-shot consume dispatch");
		check(remote.tlb_control == tlb_control::do_not_flush,
			"replayed AMD invalidation leaked into a second VMRUN");
		mock::bind(0); mock::deliver_nmis = true;
		for (std::size_t cpu = 0; cpu < mock::cpu_count; ++cpu)
		{
			mock::bind(cpu);
			slat::cache::handle();
		}
		mock::bind(0);
		std::cout << "PASS AMD TLB_CONTROL one-shot consumption and forwarded-exit replay\n";
	}
#endif

	void queue_regressions()
	{
		clean(); mock::bind(1); check(push(1), "first push"); mock::bind(0);
		bool injected = false;
		mock::on_copy = [&](void* dest, const void*, std::size_t) {
			const auto address = reinterpret_cast<std::uintptr_t>(dest);
			const auto begin = reinterpret_cast<std::uintptr_t>(buffer.data());
			if (!injected && address >= begin && address < begin + buffer.size() * sizeof(capture::rec))
			{
				injected = true; mock::bind(1); check(push(2), "concurrent push"); mock::bind(0);
			}
		};
		check(flush(1) == 1, "first flush"); mock::on_copy = nullptr;
		check(injected && capture::count() == 1, "producer head overwritten by consumer");
		check(flush() == 1 && buffer[0].va == 2, "concurrent record missing");
		check(push(3), "failed-copy setup");
		check(capture::flush(mock::guest_root, 0x300000000000, 1) == 0 && capture::count() == 1, "failed copy consumed record");
		check(capture::flush(mock::guest_root, UINTPTR_MAX - 1, UINT16_MAX) == 0, "destination overflow accepted");
		bool nested = false;
		mock::on_copy = [&](void*, const void*, std::size_t) {
			if (!nested) { nested = true; check(flush() == 0 && !capture::reset(), "overlapping consumer admitted"); }
		};
		check(flush() == 1, "failed record could not be retried"); mock::on_copy = nullptr;
		for (unsigned cpu = 0; cpu < 2; ++cpu)
		{
			mock::bind(cpu);
			for (unsigned n = 0; n < capture::per_cpu_capacity; ++n) check(push(n), "ring capacity");
		}
		mock::bind(0);
		check(flush(UINT16_MAX) == capture::max_flush_records, "unbounded hypercall copy");
		std::set<std::uint32_t> owners;
		for (const auto& r : buffer) owners.insert(r.flags);
		check(owners.size() == 2, "low CPU ring starved another ring");
		drain(); const auto lost = capture::dropped(); mock::bind(capture::max_cpus);
		check(!push() && capture::dropped() == lost + 1 && capture::count() == 0, "invalid CPU shared ring zero");
		mock::bind(0);
		std::cout << "PASS queue interleaving, copy failure, exclusive consumer, bound and fairness\n";
	}

	void queue_stress()
	{
		clean();
		constexpr unsigned cpus = 16, attempts = 2000;
		std::atomic<unsigned> remaining{cpus}, accepted{}, rejected{};
		const auto lost_before = capture::dropped();
		std::vector<std::thread> threads;
		for (unsigned id = 0; id < cpus; ++id)
			threads.emplace_back([&, id] {
				mock::bind(id);
				std::array<std::uint8_t, page_size> data;
				data.fill(static_cast<std::uint8_t>(id));
				for (unsigned n = 0; n < attempts; ++n)
				{
					while (!capture::push(id, n, id, n, 0, data.data()))
					{
						++rejected; std::this_thread::yield();
					}
					++accepted;
					if ((n & 15) == 0) std::this_thread::yield();
				}
				--remaining;
			});
		std::set<std::uint64_t> sequences;
		std::size_t consumed = 0;
		bool valid = true;
		while (remaining.load() || capture::count())
		{
			const auto n = flush();
			for (std::size_t k = 0; k < n; ++k)
			{
				const auto& r = buffer[k];
				valid &= r.flags < cpus && r.cr3 == r.flags && sequences.insert(r.seq).second;
				valid &= std::all_of(std::begin(r.data), std::end(r.data), [&](auto v) { return v == r.flags; });
			}
			consumed += n;
			if (!n) std::this_thread::yield();
		}
		for (auto& thread : threads) thread.join();
		check(valid && consumed == accepted && accepted == cpus * attempts, "concurrent record corruption/loss/duplicate");
		check(capture::dropped() - lost_before == rejected, "overflow statistics lost updates");
		check(capture::count() == 0, "queue not empty after drain");
		std::cout << "PASS 16 producers x 2000 records: accepted=" << accepted << " consumed=" << consumed << " rejected=" << rejected << '\n';
	}

	void trap_regressions()
	{
		clean(); constexpr phys_addr page = 0x2000;
		auto* pte = prepare(page);
		const auto original = flags(pte);
		check(slat::hook::trap_exec(page), "arm failed");
		check(nx(pte) && (flags(pte) & 2) == (original & 2), "execute trap changed write permission");
		const auto allocations = mock::allocations.load();
		check(slat::hook::trap_capture(page), "capture failed");
		check(flags(pte) == original && mock::allocations == allocations, "fault restored wrong permissions or allocated");
		for (unsigned i = 0; i < 10000; ++i) check(slat::hook::trap_capture(page), "repeated stale fault");
		check(capture::count() == 1 && slat::hook::trap_captured() == 1, "one-shot captured more than once");
		drain(); clean();
		pte = prepare(page); flags(pte, flags(pte) & ~2ull);
		const auto readonly = flags(pte);
		check(slat::hook::trap_exec(page) && slat::hook::trap_capture(page), "read-only executable capture");
		check(flags(pte) == readonly, "restore granted write access");
		clean(); prepare(page);
		unsigned queries = 0; bool early = false;
		std::function<void()> schedule;
		schedule = [&] {
			if (++queries == 3) early = slat::hook::trap_capture(page);
			else mock::on_mode_query = schedule;
		};
		mock::on_mode_query = schedule;
		check(slat::hook::trap_exec(page), "early-fault arm"); mock::on_mode_query = nullptr;
		check(early && !nx(pte) && capture::count() == 1, "early capture left an orphan NX page");
		clean(); prepare(page);
		for (unsigned i = 0; i < capture::per_cpu_capacity; ++i) check(push(), "fill ring");
		const auto lost = slat::hook::trap_stats().lost;
		check(slat::hook::trap_exec(page) && slat::hook::trap_capture(page), "full ring must still restore execution");
		check(slat::hook::trap_captured() == 0 && slat::hook::trap_stats().lost == lost + 1, "dropped capture reported as success");
		drain();
		check(slat::hook::trap_exec(page) && nx(pte) && slat::hook::trap_capture(page), "lost page could not be retried");
		check(capture::count() == 1, "retry did not produce a record");
		clean();
		std::cout << "PASS NX-only, original permissions, 10000 stale faults, early fault and overflow retry\n";
	}

	void lifetime_and_collisions()
	{
		clean(); constexpr phys_addr a = 0x3000, b = a + (8192ull << page_shift);
		auto* pte = prepare(a);
		check(slat::hook::trap_exec(a) && slat::hook::trap_capture(a), "captured unmap setup");
		const auto failures = slat::hook::trap_stats().restore_failures;
		flags(pte, 0);
		check(slat::hook::trap_clear_all() && slat::hook::trap_count() == 0,
			"captured unmapped page could not retire");
		check(flags(pte) == 0 && slat::hook::trap_stats().restore_failures == failures,
			"captured unmap was modified or counted as a restore failure");
		clean(); pte = prepare(a);
		check(slat::hook::trap_exec(a), "armed replacement setup");
		const auto replacement = flags(pte) ^ (1ull << page_shift);
		flags(pte, replacement);
		check(slat::hook::trap_clear_all() && slat::hook::trap_count() == 0,
			"replaced armed mapping stranded trap metadata");
		check(flags(pte) == replacement && slat::hook::trap_stats().restore_failures == failures,
			"replacement mapping was widened or counted as a restore failure");
		clean();
		prepare(a); prepare(b);
		check(slat::hook::trap_exec(a) && slat::hook::trap_exec(b), "collision setup");
		check(slat::hook::trap_remove(a) && slat::hook::trap_capture(b), "deletion broke hash probe chain");
		clean(); prepare(a); prepare(b);
		check(slat::hook::trap_exec(a), "clear-during-copy setup");
		bool injected = false, accepted = false;
		mock::on_copy = [&](void*, const void* src, std::size_t size) {
			if (!injected && src == source_page && size == page_size)
			{
				injected = true;
				slat::hook::trap_clear_all();
				accepted = slat::hook::trap_exec(b);
			}
		};
		check(slat::hook::trap_capture(a), "retiring reader lost its metadata"); mock::on_copy = nullptr;
		check(injected && accepted && !nx(mock::leaf(a)) && nx(mock::leaf(b)), "slot reused under active reader");
		clean();
		for (unsigned batch = 0; batch < 270; ++batch)
		{
			for (unsigned n = 0; n < 32; ++n)
			{
				const auto page = 0x10000000ull + (batch * 32ull + n) * page_size;
				prepare(page);
				check(slat::hook::trap_exec(page, false), "retired table capacity leaked");
			}
			slat::cache::flush_all();
			slat::hook::trap_clear_all();
			check(slat::hook::trap_count() == 0, "batch clear left active traps");
		}
		std::cout << "PASS collision deletion, clear-during-copy grace period and 8640 distinct-page reuse\n";
	}

	void failure_and_dispatch()
	{
		clean(); constexpr phys_addr page = 0x4000;
		auto* pte = prepare(page); const auto original = flags(pte);
		check(slat::hook::trap_exec(page), "restore failure setup");
		flags(pte, flags(pte) ^ 2);
		const auto failures = slat::hook::trap_stats().restore_failures;
		check(!slat::hook::trap_capture(page), "failed restoration swallowed exit");
		check(slat::hook::trap_stats().restore_failures == failures + 1, "restore failure not counted");
		flags(pte, original); clean();
		mock::fault = {.gpa=page, .access=page_perm::execute, .caused_by_translation=true, .qualification=0x11};
		vcpu::curr().retry_gpa = UINT64_MAX;
		check(slat::violation::handle() && !slat::violation::handle(), "unknown stale fault retried without bound");
		const auto after = flags(pte);
		flags(pte, (after & ~arch::slat_execute_mask) | arch::slat_nx_bits);
		check(!slat::violation::handle() && nx(pte), "unknown non-executable page was made executable");
		flags(pte, original);
		check(slat::hook::trap_exec(page), "dispatch trap setup");
		mock::reason = exit_reason::slat_violation;
		const auto calls = mock::original_calls.load();
		check(vmexit::detour(0x100, 0, 0, 0) == handled_result, "owned fault resume result");
		check(mock::original_calls == calls && !nx(pte), "owned fault reached Hyper-V before repair");
		clean();
		mock::fault.access = page_perm::write;
		check(vmexit::detour(0x100, 0, 0, 0) == 0x107 && mock::original_calls == calls + 1, "foreign write fault not forwarded");
		mock::fault.access = page_perm::execute; mock::fault.caused_by_translation = false;
		check(!slat::violation::handle(), "non-permission/page-walk fault intercepted");
		mock::root.flags ^= 0x1000; mock::fault.caused_by_translation = true;
		check(!slat::violation::handle(), "foreign NPT/EPT root intercepted"); mock::bind(0);
		std::cout << "PASS restoration failure, bounded stale retry, unknown permissions and original-handler forwarding\n";
	}

	void synchronization_and_control()
	{
		clean(); constexpr phys_addr page = 0x5000;
		prepare(page); mock::deliver_nmis = false;
		const auto notifications = mock::notifications.load();
		check(slat::hook::trap_exec(page, false), "deferred arm");
		check(mock::notifications == notifications && slat::cache::pending(slat::cache::epoch()) == mock::cpu_count - 1, "deferred invalidation tracking");
		for (std::size_t cpu = 1; cpu < mock::cpu_count; ++cpu) { mock::bind(cpu); slat::cache::handle(); }
		mock::bind(0); check(slat::cache::pending(slat::cache::epoch()) == 0, "next-exit invalidation not acknowledged");
		mock::deliver_nmis = true; clean(); prepare(page);
		std::atomic<bool> locked{}, release{};
		std::thread holder([&] {
			std::unique_lock guard(slat::hook::mtx);
			locked.store(true);
			while (!release.load()) std::this_thread::yield();
		});
		while (!locked.load()) std::this_thread::yield();
		const bool arm_rejected = !slat::hook::trap_exec(page);
		const bool clear_rejected = !slat::hook::trap_clear_all();
		release.store(true); holder.join();
		check(arm_rejected, "busy control operation waited/admitted");
		check(clear_rejected, "busy clear reported completion");
		slat::hook::shadow_owner.store(1);
		check(!slat::hook::trap_exec(page), "trap raced shadow construction");
		slat::hook::shadow_owner.store(UINT64_MAX);
		slat::hook::list.push_front({page,0,0});
		check(!slat::hook::trap_exec(page), "trap overlapped a shadow hook"); slat::hook::list.clear();
		mock::cpu_count = capture::max_cpus + 1;
		check(!slat::hook::trap_exec(page), "unsupported CPU count admitted"); mock::cpu_count = 16;
		check(!slat::hook::trap_exec((1ull << 48) | page), "out-of-range GPA aliased a valid leaf");
		prepare(page);
		std::vector<std::thread> threads;
		std::atomic<unsigned> handled{};
		check(slat::hook::trap_exec(page), "same-page concurrency setup");
		for (unsigned id = 0; id < 16; ++id) threads.emplace_back([&, id] { mock::bind(id); if (slat::hook::trap_capture(page)) ++handled; });
		for (auto& thread : threads) thread.join();
		check(handled == 16 && capture::count() == 1, "same-page concurrent capture duplicated/lost/blocked");
		clean();
#if defined(MOCK_INTEL)
		mock::mbec = true;
		auto* pte = prepare(page); flags(pte, flags(pte) & ~(1ull << 10));
		check(slat::hook::trap_exec(page), "supervisor-only MBEC arm");
		check(!slat::hook::trap_capture(page, 1ull << 10), "originally denied user execution was intercepted");
		check(slat::hook::trap_capture(page, 1ull << 2), "supervisor MBEC capture");
		check(!(flags(pte) & (1ull << 10)) && (flags(pte) & (1ull << 2)), "MBEC permission restoration widened access");
		clean(); mock::mbec = false;
#endif
		std::cout << "PASS invalidation epochs, bounded control rejection, hook conflict and same-page multicore capture\n";
	}

	void paging_and_epoch_failures()
	{
		clean();
		constexpr phys_addr page = 0x80006000;
		auto* pte = prepare(page);
		auto* pml4 = reinterpret_cast<std::uint64_t*>(slat::cr3::hyperv.address_of_page_directory << page_shift);
		auto* pdpt = reinterpret_cast<std::uint64_t*>(pml4[(page >> 39) & 511] & 0x000ffffffffff000ull);
		auto* pd = reinterpret_cast<std::uint64_t*>(pdpt[(page >> 30) & 511] & 0x000ffffffffff000ull);
		auto& ancestor = pd[(page >> 21) & 511];
		const auto parent = ancestor;
		ancestor = (parent & ~arch::slat_execute_mask) | arch::slat_nx_bits;
		check(!slat::hook::trap_exec(page), "trap armed under a non-executable ancestor");
		ancestor = parent;
		const auto original = flags(pte);
		check(slat::hook::trap_exec(page), "A/D preservation setup");
		std::atomic_ref<std::uint64_t>(pte->flags).fetch_or(arch::slat_ad_mask);
		check(slat::hook::trap_capture(page) && flags(pte) == (original | arch::slat_ad_mask), "hardware A/D bits overwritten");
		clean();
		check(slat::hook::trap_exec(page), "ancestor change setup");
		ancestor = (parent & ~arch::slat_execute_mask) | arch::slat_nx_bits;
		check(!slat::hook::trap_capture(page), "ancestor denial caused a handled-without-progress loop");
		ancestor = parent; clean();

		constexpr phys_addr large_page = 0x40000000;
		prepare(large_page);
		arch::slat_pdpte_1gb large{};
		large.flags = 7;
#if defined(MOCK_INTEL)
		large.user_mode_execute = 1;
#endif
		large.large_page = 1;
		large.page_frame_number = 8;
		pdpt[(large_page >> 30) & 511] = large.flags;
		mock::allocation_fails = true;
		check(!slat::hook::trap_exec(large_page + 0x123000), "allocation failure accepted");
		check(pdpt[(large_page >> 30) & 511] == large.flags, "failed split damaged large mapping");
		mock::allocation_fails = false;
		const auto allocations = mock::allocations.load();
		check(slat::hook::trap_exec(large_page + 0x123000), "1GB/2MB split failed");
		check(mock::allocations == allocations + 2, "split used unexpected allocations");
		pte = mock::leaf(large_page + 0x123000);
		check(pte && pte->page_frame_number == ((8ull << 30) + 0x123000) >> page_shift, "split changed physical mapping");
		clean();

		mock::deliver_nmis = false; mock::bind(0); slat::cache::flush_all(false);
		mock::bind(1);
		mock::on_flush = [] { slat::cache::flush_all(false); };
		slat::cache::handle();
		check(vcpu::curr().observed_epoch.load() == slat::cache::epoch(), "nested invalidation regressed acknowledgment");
		for (std::size_t cpu = 0; cpu < mock::cpu_count; ++cpu) { mock::bind(cpu); slat::cache::handle(); }
		mock::bind(0); mock::deliver_nmis = true;
		check(slat::cache::pending(slat::cache::epoch()) == 0, "epoch remained pending after all CPUs acknowledged");
		std::cout << "PASS parent permissions, A/D preservation, allocation failure, large-page split and nested epoch acknowledgment\n";
	}

	void arm_clear_stress()
	{
		clean(); constexpr phys_addr page = 0x9000;
		auto* pte = prepare(page); const auto original = flags(pte);
		std::vector<std::thread> threads;
		for (unsigned id = 0; id < 16; ++id)
			threads.emplace_back([id] {
				mock::bind(id);
				for (unsigned n = 0; n < 1000; ++n)
				{
					if (slat::hook::trap_exec(page, false)) slat::hook::trap_capture(page);
					if ((n & 7) == 0) slat::hook::trap_clear_all();
				}
			});
		for (auto& thread : threads) thread.join();
		mock::bind(0); slat::cache::flush_all();
		check(slat::hook::trap_clear_all(), "final concurrent clear busy");
		check(flags(pte) == original && slat::hook::trap_count() == 0, "concurrent arm/capture/clear stranded NX");
		const auto records = drain();
		std::set<std::uint64_t> sequences;
		for (const auto& r : records)
			check(sequences.insert(r.seq).second && r.gpa == page && r.data[0] == 0xa5 && r.data[page_size-1] == 0xa5, "concurrent trap record corrupted");
		std::cout << "PASS 16 CPUs x 1000 same-page arm/capture/clear races\n";
	}

	void coherent_diagnostics()
	{
		std::atomic<unsigned> remaining{16};
		std::vector<std::thread> threads;
		for (unsigned id = 0; id < 16; ++id)
			threads.emplace_back([&, id] {
				mock::bind(id);
				mock::rip = (id + 1) ^ 0xabcd;
				::slat_violation fault{.gpa=id+1, .qualification=(id+1)^0x4321};
				for (unsigned n = 0; n < 1000; ++n) slat::hook::trap_note_unhandled(fault);
				--remaining;
			});
		bool valid = true;
		do
		{
			const auto s = slat::hook::trap_stats();
			if (s.last_gpa > 0 && s.last_gpa <= 16)
				valid &= s.last_rip == (s.last_gpa ^ 0xabcd) && s.last_qualification == (s.last_gpa ^ 0x4321);
		} while (remaining.load());
		for (auto& thread : threads) thread.join();
		check(valid, "diagnostic snapshot mixed different faults");
		std::cout << "PASS concurrent fault diagnostics remain coherent\n";
	}
}

int main()
{
	try
	{
		mock::setup(); source_page = mock::allocate_page();
		std::memset(source_page, 0xa5, page_size);
		bytes.fill(0x37); mock::map_guest_buffer(buffer.data(), buffer.size() * sizeof(capture::rec));
		initialization_and_dispatch();
#if !defined(MOCK_INTEL)
		amd_tlb_control_lifecycle();
#endif
		queue_regressions(); queue_stress(); trap_regressions();
		lifetime_and_collisions(); failure_and_dispatch(); synchronization_and_control();
		paging_and_epoch_failures(); arm_clear_stress(); coherent_diagnostics();
		std::cout << "ALL PASS, assertions=" << checks << '\n';
		return 0;
	}
	catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
