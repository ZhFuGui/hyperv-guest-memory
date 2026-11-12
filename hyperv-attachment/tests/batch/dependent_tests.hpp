#pragma once
#include "../../src/vmexit/hypercall/dependent.hpp"

namespace test
{
	using dep_kind = hc::dependent_read_kind;
	using dep_status = hc::dependent_read_status;
	hc::dependent_read_item dep_pointer(std::uint64_t address, std::uint16_t parent = hc::dependent_direct)
	{ return {address, 0, 8, parent, dep_kind::pointer, 0}; }
	hc::dependent_read_item dep_copy(std::uint64_t address, std::uint64_t dest, std::uint32_t size,
	                                  std::uint16_t parent = hc::dependent_direct)
	{ return {address, dest, size, parent, dep_kind::copy, 0}; }
	struct dependent_fixture : fixture
	{
		hc::dependent_read_item* q = reinterpret_cast<hc::dependent_read_item*>(items);
		hc::dependent_read_result* out = reinterpret_cast<hc::dependent_read_result*>(mask);
		std::uint8_t* src = map(source, source_va);
		std::uint8_t* dst = map(caller, output_va);
		void pointer(std::uint64_t value) { std::memcpy(src, &value, 8); }
		bool run(std::uint64_t n) { return hc::dependent::read(source.flags, request_va, n, mask_va); }
	};

	void dependent_basics()
	{
		dependent_fixture f;
		f.pointer(source_va + 64);
		const std::uint64_t second = source_va + 128; std::memcpy(f.src + 64, &second, 8);
		std::memset(f.src + 128, 0x37, 32); std::memset(f.src + 96, 0xA9, 16);
		f.q[0] = dep_pointer(source_va); f.q[1] = dep_pointer(0, 0);
		f.q[2] = dep_copy(0, output_va, 32, 1); f.q[3] = dep_copy(32, output_va + 64, 16, 0);
		f.q[4] = dep_copy(source_va + 96, output_va + 80, 16);
		const auto allocations = slab_allocs.load();
		check(f.run(5) && std::all_of(f.out->status, f.out->status + 5,
		      [](auto s) { return s == dep_status::success; }), "dependent chain, branch and independent root");
		check(f.out->pointers[0] == source_va + 64 && f.out->pointers[1] == second && !f.out->pointers[2] &&
		      !std::memcmp(f.dst, f.src + 128, 32) && !std::memcmp(f.dst + 64, f.src + 96, 16) &&
		      !std::memcmp(f.dst + 80, f.src + 96, 16), "dependent outputs and pointer observations");
		check(slab_allocs == allocations, "dependent has no hot allocations");
		check(std::all_of(f.out->status + 5, f.out->status + 64, [](auto s) { return s == dep_status::unused; }) &&
		      std::all_of(f.out->pointers + 5, f.out->pointers + 64, [](auto p) { return p == 0; }), "dependent unused entries cleared");
		f.src[56] = 0xD1; f.q[1] = dep_copy(static_cast<std::uint64_t>(-8LL), output_va, 1, 0);
		check(f.run(2) && f.out->status[1] == dep_status::success && f.dst[0] == 0xD1, "negative fixed offset");

		// A full 64-node dependency chain, without recursion or a separate depth budget.
		for (unsigned i = 0; i < 64; ++i) {
			const auto next = source_va + (i + 1) * 8; std::memcpy(f.src + i * 8, &next, 8);
			f.q[i] = dep_pointer(i ? 0 : source_va, i ? static_cast<std::uint16_t>(i - 1) : hc::dependent_direct);
		}
		check(f.run(64) && f.out->status[63] == dep_status::success && f.out->pointers[63] == source_va + 512,
		      "64-node dependency chain");
		check(!f.run(0) && !f.run(65) && !f.run(0x100000001ULL), "dependent count validated before narrowing");
		f.q[0] = dep_pointer(source_va);
		check(!hc::dependent::read(0, request_va, 1, mask_va) &&
		      !hc::dependent::read(f.source.flags | (1ULL << 48), request_va, 1, mask_va), "dependent CR3 bounds");
		check(hc::dependent::read(f.source.flags | 0x123, request_va, 1, mask_va), "dependent PCID retained");

		for (unsigned i = 1; i < 16; ++i) { map(f.source, source_va + i * 4096); map(caller, output_va + i * 4096); }
		f.pointer(source_va + 8); f.q[0] = dep_pointer(source_va);
		f.q[1] = dep_copy(0, output_va, 65528, 0);
		check(f.run(2) && f.out->status[1] == dep_status::success, "64 KiB including pointer read accepted");
		f.dst[0] = 0xCC; ++f.q[1].size;
		check(!f.run(2) && f.dst[0] == 0xCC, "pointer bytes count towards total before writes");
		f.q[0] = dep_copy(source_va, output_va, 65536);
		check(f.run(1) && f.out->status[0] == dep_status::success, "64 KiB direct copy accepted");
	}

	void dependent_validation()
	{
		dependent_fixture f; f.src[0] = 0x59;
		const auto valid = dep_copy(source_va, output_va + 16, 8);
		for (unsigned mode = 0; mode < 19; ++mode) {
			f.q[0] = dep_copy(source_va, output_va, 8); f.q[1] = valid;
			switch (mode) {
			case 0: f.q[1].reserved = 1; break;
			case 1: f.q[1].kind = static_cast<dep_kind>(0); break;
			case 2: f.q[1].kind = static_cast<dep_kind>(255); break;
			case 3: f.q[1].parent = 1; break;
			case 4: f.q[1].parent = 2; break;
			case 5: f.q[1].parent = 65534; break;
			case 6: f.q[1].parent = 0; break; // Copy items cannot supply a pointer.
			case 7: f.q[1] = dep_pointer(source_va); f.q[1].destination_va = output_va + 16; break;
			case 8: f.q[1] = dep_pointer(source_va); f.q[1].size = 7; break;
			case 9: f.q[1].size = 0; break;
			case 10: f.q[1].size = UINT32_MAX; break;
			case 11: f.q[1].destination_va = output_va + 4; break;
			case 12: f.q[1].destination_va = request_va + 1; break;
			case 13: f.q[1].destination_va = mask_va + sizeof(*f.out) - 1; break;
			case 14: f.q[1].address = 0x800000000000ULL; break;
			case 15: f.q[1].address = 0x7FFFFFFFFFFFULL; break;
			case 16: f.q[1].destination_va = UINT64_MAX - 1; break;
			case 17: f.q[1].address = 0; break;
			case 18: f.q[1].destination_va = 0; break;
			}
			std::memset(f.dst, 0xCC, 32); std::memset(f.out, 0xAA, sizeof(*f.out));
			check(!f.run(2) && f.dst[0] == 0xCC && reinterpret_cast<std::uint8_t*>(f.out)[0] == 0xAA,
			      "invalid later descriptor rejected before any output");
		}
		f.q[0] = valid;
		check(!hc::dependent::read(f.source.flags, request_va, 1, request_va + 8), "dependent controls cannot overlap");
		check(!hc::dependent::read(f.source.flags, 0x800000000000ULL, 1, mask_va) &&
		      !hc::dependent::read(f.source.flags, request_va, 1, UINT64_MAX - 4), "dependent control ranges");
		check(!hc::dependent::read(f.source.flags, request_va + 4096, 1, mask_va), "dependent unmapped descriptors");
		cpu = static_cast<unsigned long>(vcpu::count()); check(!f.run(1), "dependent CPU bound"); cpu = 0;
		auto* const saved = vcpu::curr().batch_work; vcpu::curr().batch_work = nullptr;
		check(!f.run(1), "dependent missing workspace"); vcpu::curr().batch_work = saved;
		const auto old_caller = caller, old_slat = slat;
		caller.flags = 0; check(!f.run(1), "dependent missing caller root"); caller = old_caller;
		slat.flags = 0; check(!f.run(1), "dependent missing SLAT root"); slat = old_slat;
		check(!saved->busy.load(), "dependent guard failure releases workspace");
	}

	void dependent_failures()
	{
		dependent_fixture f;
		f.q[0] = dep_pointer(source_va); f.q[1] = dep_pointer(0, 0);
		f.q[2] = dep_copy(0, output_va, 1, 1); f.q[3] = dep_copy(source_va + 32, output_va + 8, 1);
		f.src[32] = 0x79;
		for (auto value : {0ULL, 0x800000000000ULL}) {
			f.pointer(value);
			check(f.run(4) && f.out->status[0] == dep_status::invalid_address &&
			      f.out->status[1] == dep_status::dependency_failed && f.out->status[2] == dep_status::dependency_failed &&
			      f.out->status[3] == dep_status::success && f.dst[8] == 0x79 && !f.out->pointers[0],
			      "invalid pointer skips descendants but keeps independent root");
		}
		f.q[0].address = source_va + 4096;
		check(f.run(4) && f.out->status[0] == dep_status::transfer_failed && f.out->status[2] == dep_status::dependency_failed &&
		      f.out->status[3] == dep_status::success, "unmapped pointer source fails its chain only");
		f.q[0].address = source_va;
		struct edge { std::uint64_t pointer, offset; std::uint32_t size; };
		for (const auto& e : {edge{1, UINT64_MAX, 1}, edge{1, UINT64_MAX - 1, 1}, edge{source_va, 1ULL << 63, 1},
		                     edge{UINT64_MAX, 1, 1}, edge{0x7FFFFFFFFFFFULL, 1, 1}, edge{UINT64_MAX, 0, 2}}) {
			f.pointer(e.pointer); f.q[1] = dep_copy(e.offset, output_va, e.size, 0);
			check(f.run(2) && f.out->status[0] == dep_status::success && f.out->status[1] == dep_status::invalid_address,
			      "relative wrap, INT64_MIN, null and canonical hole rejected");
		}
		map(f.source, UINT64_MAX - 4095)[4095] = 0xE3; f.pointer(UINT64_MAX);
		f.q[1] = dep_copy(0, output_va, 1, 0);
		check(f.run(2) && f.out->status[1] == dep_status::success && f.dst[0] == 0xE3, "last canonical byte through pointer");
		f.q[0] = dep_copy(source_va + 4090, output_va, 12); f.dst[0] = 0xCC; f.src[4090] = 0xA1;
		check(f.run(1) && f.out->status[0] == dep_status::transfer_failed && f.dst[0] == 0xA1, "partial source copy invalidates item");
		f.q[0] = dep_copy(source_va, output_va + 4090, 12);
		check(f.run(1) && f.out->status[0] == dep_status::transfer_failed, "partial destination copy invalidates item");
		f.q[0] = dep_copy(source_va, output_va, 1);
		check(!hc::dependent::read(f.source.flags, request_va, 1, mask_va + 4096), "dependent publication failure");
		check(!vcpu::curr().batch_work->busy.load() && f.run(1), "dependent recovers after publication failure");
		const auto gpa = *guest_leaf(f.source, source_va) & ~4095ULL;
		slat_map(gpa, hva_identity_map_size);
		check(f.run(1) && f.out->status[0] == dep_status::transfer_failed, "dependent leaf outside identity coverage");
		slat_map(gpa, gpa_hpa.at(gpa));
		const auto root_hpa = gpa_hpa.at(f.source.flags); slat_map(f.source.flags, hva_identity_map_size);
		check(f.run(1) && f.out->status[0] == dep_status::transfer_failed, "dependent source root outside identity coverage");
		slat_map(f.source.flags, root_hpa);
	}

	void dependent_snapshot_and_dispatch()
	{
		dependent_fixture f;
		auto* middle = map(f.source, source_va + 4096); auto* target = map(f.source, source_va + 8192);
		middle[0] = 0x45; target[0] = 0x93; f.pointer(source_va + 8192);
		f.q[0] = dep_pointer(source_va); f.q[1] = dep_copy(source_va + 4096, output_va, 1);
		f.q[2] = dep_copy(0, output_va + 8, 1, 0);
		const auto q = f.q[2]; bool changed = false;
		const auto middle_hpa = gpa_hpa.at(*guest_leaf(f.source, source_va + 4096) & ~4095ULL);
		std::function<void(std::uint64_t)> callback;
		callback = [&](std::uint64_t hpa) {
			if (hpa == middle_hpa) {
				f.out->pointers[0] = source_va; f.pointer(0); f.q[2].destination_va = mask_va; changed = true;
			} else on_map = callback;
		};
		on_map = callback;
		check(f.run(3) && changed && f.out->pointers[0] == source_va + 8192 && f.dst[8] == 0x93 &&
		      f.out->status[2] == dep_status::success, "private pointer and descriptor snapshots survive caller/source mutation");
		f.q[2] = q; f.pointer(source_va + 8192);
		const auto root_hpa = gpa_hpa.at(f.source.flags), saved_slat = slat.flags;
		callback = [&](std::uint64_t hpa) { if (hpa == root_hpa) slat.flags = 0; else on_map = callback; };
		on_map = callback; const bool complete = f.run(3); slat.flags = saved_slat; on_map = {};
		check(complete && f.out->status[2] == dep_status::success, "dependent one SLAT context throughout call");
		bool nested = true, nested_batch = true, nested_list = true;
		on_map = [&](std::uint64_t) {
			nested = f.run(3); nested_batch = hc::batch::read(f.source.flags, request_va, 1, mask_va);
			nested_list = hc::list_find::find(f.source.flags, request_va, mask_va);
		};
		check(f.run(3) && !nested && !nested_batch && !nested_list, "operations 11/12/13 share nonwaiting reentry guard");
		guest_regs regs{}; regs.rcx = (hc::allowed_key << 16) | 13;
		regs.rdx = f.source.flags; regs.r8 = request_va; regs.r9 = 3; regs.r10 = mask_va;
		check(hc::handle(regs) && regs.rax == 1 && f.out->status[2] == dep_status::success, "operation 13 production dispatch");
		regs.r11 = 1; check(hc::handle(regs) && !regs.rax, "operation 13 reserved argument rejected");
		regs.r11 = 0; regs.r9 = 0x100000003ULL;
		check(hc::handle(regs) && !regs.rax, "operation 13 full-width count through dispatch");
	}

	void dependent_split()
	{
		dependent_fixture f;
		f.q[0] = dep_pointer(source_va + 4092); f.q[1] = dep_copy(0, output_va + 4092, 8, 0);
		const auto control = request_va + 4090; const auto bytes = 2 * sizeof(*f.q);
		std::memcpy(reinterpret_cast<std::uint8_t*>(f.q) + 4090, f.q, 6);
		check(!hc::dependent::read(f.source.flags, control, 2, mask_va), "partial descriptor snapshot rejected");
		auto* second = map(caller, request_va + 4096);
		std::memcpy(second, reinterpret_cast<std::uint8_t*>(f.q) + 6, bytes - 6);
		map(f.source, source_va + 4096); map(caller, output_va + 4096);
		const std::uint64_t pointer = source_va + 32, data = 0x123456789ABCDEF0ULL;
		check(mem::guest::write_virt(f.source, source_va + 4092, &pointer, 8) &&
		      mem::guest::write_virt(f.source, pointer, &data, 8), "prepare split pointer");
		map(caller, mask_va + 4096); const auto result_va = mask_va + 4090;
		check(hc::dependent::read(f.source.flags, control, 2, result_va), "split controls, pointer, output and publication");
		hc::dependent_read_result result{}; std::uint64_t observed = 0;
		check(mem::guest::read_virt(caller, result_va, &result, sizeof(result)) && result.status[1] == dep_status::success &&
		      result.pointers[0] == pointer && mem::guest::read_virt(caller, output_va + 4092, &observed, 8) && observed == data,
		      "split dependent payload preserved across nonidentity pages");
		*guest_leaf(caller, mask_va + 4096) = 0;
		check(!hc::dependent::read(f.source.flags, control, 2, result_va), "partial dependent publication is failure");
		*guest_leaf(f.source, source_va + 4096) = 0;
		check(f.run(2) && f.out->status[0] == dep_status::transfer_failed && !f.out->pointers[0] &&
		      f.out->status[1] == dep_status::dependency_failed, "partial pointer never escapes or feeds child");
	}

	void dependent_random()
	{
		dependent_fixture f;
		std::uint32_t rng = 0x13BADC0D;
		auto random = [&]() { rng = rng * 1664525 + 1013904223; return rng; };
		for (unsigned run = 0; run < 256; ++run) {
			// Host reference model: a single page of pointer cells, some null,
			// noncanonical or pointing to an absent page. No production walker used.
			for (unsigned cell = 0; cell < 512; ++cell) {
				const auto r = random();
				const std::uint64_t value = (r & 15) == 0 ? 0 : (r & 15) == 1 ? 0x800000000000ULL
				                          : (r & 15) == 2 ? source_va + 4096 : source_va + ((r >> 8) & 511) * 8;
				std::memcpy(f.src + cell * 8, &value, 8);
			}
			hc::dependent_read_result expected{};
			std::array<std::uint64_t, 64> copied{};
			std::vector<std::uint16_t> parents;
			std::memset(f.dst, 0xCC, 512);
			for (std::uint16_t i = 0; i < 64; ++i) {
				const auto r = random(); const bool pointer = (r >> 16) & 1;
				const bool dependent = !parents.empty() && (r & 3) != 0;
				const auto parent = dependent ? parents[(r >> 8) % parents.size()] : hc::dependent_direct;
				const auto root = source_va + ((r >> 20) & 511) * 8;
				f.q[i] = pointer ? dep_pointer(dependent ? 0 : root, parent)
				                 : dep_copy(dependent ? 0 : root, output_va + i * 8, 8, parent);
				if (pointer) parents.push_back(i);
				if (dependent && expected.status[parent] != dep_status::success) {
					expected.status[i] = dep_status::dependency_failed; continue;
				}
				const auto va = dependent ? expected.pointers[parent] : root;
				if (va < source_va || va - source_va > 4088) { expected.status[i] = dep_status::transfer_failed; continue; }
				std::uint64_t value = 0; std::memcpy(&value, f.src + (va - source_va), 8);
				if (pointer && (!value || value == 0x800000000000ULL)) {
					expected.status[i] = dep_status::invalid_address; continue;
				}
				expected.status[i] = dep_status::success;
				if (pointer) expected.pointers[i] = value; else copied[i] = value;
			}
			check(f.run(64) && !std::memcmp(f.out, &expected, sizeof(expected)), "random dependent graph statuses and pointers");
			for (unsigned i = 0; i < 64; ++i) if (f.q[i].kind == dep_kind::copy) {
				std::uint64_t actual = 0; std::memcpy(&actual, f.dst + i * 8, 8);
				check(actual == (expected.status[i] == dep_status::success ? copied[i] : 0xCCCCCCCCCCCCCCCCULL),
				      "random dependent graph successful data and skipped outputs");
			}
		}
	}

	void dependent_output_guards()
	{
		dependent_fixture f; f.src[0] = 0x73;
		constexpr std::uint64_t kernel_va = 0xFFFF800000000000ULL;
		auto* kernel = map(caller, kernel_va); kernel[0] = 0xCC;
		f.q[0] = dep_copy(source_va, kernel_va, 1);
		check(!f.run(1) && kernel[0] == 0xCC, "dependent rejects kernel output before write");
		f.q[0] = dep_copy(source_va, output_va, 1);
		// A low canonical address can still map a read-only or supervisor page.
		for (const auto bit : {2ULL, 4ULL}) {
			auto* leaf = guest_leaf(caller, output_va); const auto saved = *leaf; *leaf &= ~bit;
			f.dst[0] = 0xCC;
			check(f.run(1) && f.out->status[0] == dep_status::transfer_failed && f.dst[0] == 0xCC,
			      "dependent refuses read-only or supervisor destination leaf");
			*leaf = saved;
			auto* result_leaf = guest_leaf(caller, mask_va); const auto result_saved = *result_leaf; *result_leaf &= ~bit;
			std::memset(f.out, 0xAA, sizeof(*f.out));
			check(!f.run(1) && reinterpret_cast<std::uint8_t*>(f.out)[0] == 0xAA, "dependent result requires writable user page");
			*result_leaf = result_saved;
		}
		// Do not bypass a second-level write-protected/COW mapping either.
		for (const auto va : {output_va, mask_va}) {
			const auto gpa = *guest_leaf(caller, va) & ~4095ULL;
			auto node = slat.flags;
			for (const auto shift : {39, 30, 21}) node = table(node)[(gpa >> shift) & 511] & ~4095ULL;
			auto& leaf = table(node)[(gpa >> 12) & 511]; const auto saved = leaf; leaf &= ~2ULL;
			f.dst[0] = 0xCC; std::memset(f.out, 0xAA, sizeof(*f.out));
			const bool published = f.run(1);
			check(va == output_va ? published && f.out->status[0] == dep_status::transfer_failed && f.dst[0] == 0xCC
			                      : !published && reinterpret_cast<std::uint8_t*>(f.out)[0] == 0xAA,
			      "dependent checked writes respect SLAT write protection"); leaf = saved;
		}
		// Each ancestor contributes to effective guest write/user permission.
		auto node = caller.flags & ~4095ULL;
		for (const auto shift : {39, 30, 21}) {
			auto& entry = table(gpa_hpa.at(node))[(output_va >> shift) & 511]; const auto saved = entry;
			for (const auto bit : {2ULL, 4ULL}) {
				entry = saved & ~bit; f.dst[0] = 0xCC;
				const bool published = f.run(1);
				check(f.dst[0] == 0xCC && (!published || f.out->status[0] == dep_status::transfer_failed),
				      "dependent effective output permissions include ancestors");
			}
			entry = saved; node = saved & ~4095ULL;
		}
		auto* control_leaf = guest_leaf(caller, request_va); const auto control_saved = *control_leaf;
		*control_leaf &= ~2ULL;
		check(f.run(1) && f.out->status[0] == dep_status::success, "dependent descriptors may be read-only user memory");
		*control_leaf = control_saved & ~4ULL; f.dst[0] = 0xCC;
		check(!f.run(1) && f.dst[0] == 0xCC, "dependent rejects supervisor descriptors"); *control_leaf = control_saved;
		auto* source = map(f.source, kernel_va); source[0] = 0x91; *guest_leaf(f.source, kernel_va) &= ~6ULL;
		f.q[0] = dep_copy(kernel_va, output_va, 1);
		check(f.run(1) && f.out->status[0] == dep_status::success && f.dst[0] == 0x91,
		      "dependent kernel read-only SOURCE remains readable");
		kernel[0] = 0x5E; *guest_leaf(caller, kernel_va) &= ~6ULL;
		check(hc::dependent::read(caller.flags, request_va, 1, mask_va) && f.out->status[0] == dep_status::success &&
		      f.dst[0] == 0x5E, "dependent separates read/write permissions even when both CR3s are identical");
		check(!hc::dependent::read(f.source.flags, kernel_va, 1, mask_va) &&
		      !hc::dependent::read(f.source.flags, request_va, 1, kernel_va), "dependent controls must be user addresses");
		for (const auto shift : {30, 21}) {
			dependent_fixture large; large.src[0] = 0x36;
			large.q[0] = dep_copy(source_va, output_va, 1);
			auto table_gpa = caller.flags;
			for (const auto level : {39, 30}) {
				if (level == shift) break;
				table_gpa = table(gpa_hpa.at(table_gpa))[(output_va >> level) & 511] & ~4095ULL;
			}
			auto& leaf = table(gpa_hpa.at(table_gpa))[(output_va >> shift) & 511];
			const auto output_hpa = gpa_hpa.at(*guest_leaf(caller, output_va) & ~4095ULL);
			slat_map(0x40000000, output_hpa); leaf = 0x40000087;
			for (const auto bit : {2ULL, 4ULL}) {
				leaf = 0x40000087 & ~bit; large.dst[0] = 0xCC;
				check(large.run(1) && large.out->status[0] == dep_status::transfer_failed && large.dst[0] == 0xCC,
				      "dependent effective permissions include large-page leaf");
			}
			leaf = 0x40000087;
			check(large.run(1) && large.out->status[0] == dep_status::success && large.dst[0] == 0x36,
			      "dependent writable user large-page output");
		}
		{
			dependent_fixture changing;
			auto* source2 = map(changing.source, source_va + 4096); auto* dest2 = map(caller, output_va + 4096);
			std::memset(changing.src, 0x29, 4096); std::memset(source2, 0x57, 4096); std::memset(dest2, 0xCC, 4096);
			changing.q[0] = dep_copy(source_va, output_va, 8192);
			auto* const dest_leaf = guest_leaf(caller, output_va + 4096);
			const auto source2_hpa = gpa_hpa.at(*guest_leaf(changing.source, source_va + 4096) & ~4095ULL);
			std::function<void(std::uint64_t)> protect;
			protect = [&](std::uint64_t hpa) { if (hpa == source2_hpa) *dest_leaf &= ~2ULL; else on_map = protect; };
			on_map = protect;
			check(changing.run(1) && changing.out->status[0] == dep_status::transfer_failed && changing.dst[0] == 0x29 &&
			      dest2[0] == 0xCC, "dependent rechecks write permissions at each page, not just preflight");
		}
	}

	void dependent_sdk()
	{
		std::array<hc::dependent_read_item, 64> items{}; hc::dependent_read_result result{};
		sdk_calls = 0; sdk_result = 1;
		check(hc::read_virtual_dependent_batch(0x12345000, items.data(), 64, result) && sdk_calls == 1 &&
		      result.status[0] == dep_status::success, "dependent SDK single call");
		check(sdk_params.info.type == hc::type::virt_read_dependent_batch && sdk_params.info.key == hc::allowed_key &&
		      sdk_params.args[0] == 0x12345000 && sdk_params.args[1] == reinterpret_cast<std::uintptr_t>(items.data()) &&
		      sdk_params.args[2] == 64 && sdk_params.args[3] == reinterpret_cast<std::uintptr_t>(&result) && !sdk_params.args[4],
		      "dependent SDK ABI arguments");
		for (auto value : {0ULL, 2ULL}) {
			sdk_result = value;
			check(!hc::read_virtual_dependent_batch(1, items.data(), 1, result) &&
			      std::all_of(reinterpret_cast<std::uint8_t*>(&result), reinterpret_cast<std::uint8_t*>(&result) + sizeof(result),
			                  [](auto b) { return b == 0; }), "dependent SDK clears incomplete publication");
		}
		const auto calls = sdk_calls;
		check(!hc::read_virtual_dependent_batch(1, nullptr, 1, result) &&
		      !hc::read_virtual_dependent_batch(1, items.data(), 0, result) &&
		      !hc::read_virtual_dependent_batch(1, items.data(), 65, result) && sdk_calls == calls, "dependent SDK rejects counts locally");
		std::memset(&result, 0xA7, sizeof(result));
		check(!hc::read_virtual_dependent_batch(1, reinterpret_cast<hc::dependent_read_item*>(&result), 1, result) &&
		      reinterpret_cast<std::uint8_t*>(&result)[0] == 0xA7 && sdk_calls == calls, "dependent SDK rejects alias before clearing");
		check(!hc::read_virtual_dependent_batch(1, reinterpret_cast<const hc::dependent_read_item*>(0xFFFF800000000000ULL),
		      1, result) && sdk_calls == calls, "dependent SDK refuses kernel controls without dereferencing");
	}
}
