#pragma once
#include "../../src/vmexit/hypercall/list_find.hpp"
#include "../../src/vmexit/hypercall/dependent.hpp"

namespace test
{
	struct list_fixture : fixture
	{
		static constexpr std::uint64_t head_va = source_va + 4096;
		static constexpr std::uint32_t link_offset = 32, key_offset = 16;
		struct pair { std::uint64_t next, prev; };
		std::vector<std::uint64_t> objects;
		std::vector<std::uint8_t*> nodes;
		pair* head;
		hc::list_find_request* q;
		hc::list_find_result* out;
		explicit list_fixture(unsigned count)
		{
			q = reinterpret_cast<hc::list_find_request*>(items);
			out = reinterpret_cast<hc::list_find_result*>(mask);
			*q = {}; q->version = 1; q->struct_size = sizeof(*q); q->head_va = head_va;
			q->link_offset = link_offset; q->key_offset = key_offset; q->key_size = 8; q->window_size = 128;
			head = reinterpret_cast<pair*>(map(source, head_va));
			std::uint8_t* page = nullptr;
			for (unsigned i = 0; i < count; ++i) {
				const auto va = source_va + 0x10000 + i * 256;
				if (!(i % 16)) page = map(source, va);
				objects.push_back(va); nodes.push_back(page + (i % 16) * 256);
				const std::uint64_t id = 1000 + i; std::memcpy(nodes.back() + key_offset, &id, 8);
			}
			for (unsigned i = 0; i < count; ++i) {
				const pair value{i + 1 < count ? link(i + 1) : head_va, i ? link(i - 1) : head_va};
				std::memcpy(nodes[i] + link_offset, &value, sizeof(value));
			}
			*head = {count ? link(0) : head_va, count ? link(count - 1) : head_va};
		}
		std::uint64_t link(unsigned i) const { return objects[i] + link_offset; }
		pair& entry(unsigned i) { return *reinterpret_cast<pair*>(nodes[i] + link_offset); }
		void key(std::uint64_t id) { std::memcpy(q->key, &id, 8); }
		bool find() { return hc::list_find::find(source.flags, request_va, mask_va); }
		void sanitized()
		{
			check(out->reserved == 0, "list result reserved zero");
			if (out->status != hc::list_find_status::found) {
				check(!out->object_va && !out->window_size && std::all_of(std::begin(out->window), std::end(out->window),
				      [](auto b) { return b == 0; }), "list non-found data cleared");
			} else check(std::all_of(out->window + out->window_size, out->window + 1024, [](auto b) { return b == 0; }),
			             "list unused bytes cleared");
			if (out->status != hc::list_find_status::more) check(!out->next_link && !out->previous_link, "list cursor only for more");
		}
	};

	void list_basics()
	{
		using status = hc::list_find_status;
		{
			list_fixture f(0); check(f.find() && f.out->status == status::not_found && !f.out->visited, "empty list"); f.sanitized();
		}
		list_fixture f(1300); f.key(1000);
		const auto allocations = slab_allocs.load();
		check(f.find() && f.out->status == status::found && f.out->object_va == f.objects[0] && f.out->visited == 1,
		      "list first node and default budget"); f.sanitized();
		check(!std::memcmp(f.out->window, f.nodes[0], 128) && slab_allocs == allocations, "list window and no hot allocation");
		f.key(2299);
		check(f.find() && f.out->status == status::more && f.out->visited == 1024 && f.out->next_link == f.link(1024) &&
		      f.out->previous_link == f.link(1023), "default 1024-node budget"); f.sanitized();
		f.q->resume_link = f.out->next_link; f.q->expected_prev = f.out->previous_link;
		check(f.find() && f.out->status == status::found && f.out->object_va == f.objects.back() && f.out->visited == 276,
		      "stateless continuation finds last node"); f.sanitized();
		f.q->resume_link = f.q->expected_prev = 0; f.q->node_budget = 4096;
		check(f.find() && f.out->status == status::found && f.out->visited == 1300, "large caller-selected budget");
		f.key(999999);
		check(f.find() && f.out->status == status::not_found && f.out->visited == 1300, "miss reaches sentinel"); f.sanitized();
		f.key(1000); f.q->window_offset = 8; f.q->window_size = 120;
		check(f.find() && !std::memcmp(f.out->window, f.nodes[0] + 8, 120), "nonzero window offset");
		f.q->key_size = 16; std::memcpy(f.q->key, f.nodes[0] + 16, 16);
		check(f.find() && f.out->status == status::found, "16-byte exact match");
		f.q->key[15] ^= 1;
		check(f.find() && f.out->status == status::not_found, "all key bytes compared");
		{
			list_fixture one(1); one.key(1000); one.q->window_size = 1024;
			check(one.find() && one.out->status == status::found && one.out->window_size == 1024, "maximum window");
		}
		{
			list_fixture limit(4097); limit.key(5096); limit.q->node_budget = 4096;
			check(limit.find() && limit.out->status == status::more && limit.out->visited == 4096, "hard node limit");
			limit.q->resume_link = limit.out->next_link; limit.q->expected_prev = limit.out->previous_link;
			check(limit.find() && limit.out->status == status::found && limit.out->visited == 1, "hard-limit continuation");
			limit.q->node_budget = 4097; check(!limit.find(), "excess node budget rejected");
		}
	}

	void list_failures()
	{
		using status = hc::list_find_status;
		list_fixture f(3); f.key(1002); const auto valid = *f.q;
		for (unsigned mode = 0; mode < 10; ++mode) {
			*f.q = valid;
			switch (mode) {
			case 0: f.q->version = 0; break;
			case 1: --f.q->struct_size; break;
			case 2: f.q->window_size = 0; break;
			case 3: f.q->window_size = 1025; break;
			case 4: f.q->key_size = 0; break;
			case 5: f.q->key_size = 17; break;
			case 6: f.q->window_offset = 17; break;
			case 7: f.q->link_offset = UINT32_MAX - 7; break;
			case 8: f.q->expected_prev = f.link(0); break;
			case 9: f.q->head_va = 0x0000800000000000ULL; break;
			}
			std::memset(f.out, 0xAA, sizeof(*f.out));
			check(!f.find() && reinterpret_cast<std::uint8_t*>(f.out)[0] == 0xAA, "invalid list request leaves output untouched");
		}
		*f.q = valid;
		check(!hc::list_find::find(f.source.flags, request_va, request_va + 16), "list control overlap rejected");
		check(!hc::list_find::find(f.source.flags, request_va + 4096, mask_va), "list missing request");
		const auto original = f.entry(0);
		f.entry(0).next = f.link(0);
		check(f.find() && f.out->status == status::inconsistent, "list self loop rejected"); f.sanitized();
		f.entry(0) = original; f.entry(1).prev = f.head_va;
		check(f.find() && f.out->status == status::inconsistent, "broken adjacent backlink rejected");
		f.entry(1).prev = f.link(0);
		f.q->node_budget = 1;
		check(f.find() && f.out->status == status::more, "one-node continuation boundary");
		f.q->resume_link = f.out->next_link; f.q->expected_prev = f.out->previous_link;
		f.entry(0).next = f.link(2); f.entry(2).prev = f.link(0);
		check(f.find() && f.out->status == status::inconsistent, "unlink invalidates cursor"); f.sanitized();
		*f.q = valid; f.entry(0) = original; f.entry(2).prev = f.link(1);
		*guest_leaf(f.source, f.objects[0]) &= ~1ULL;
		check(f.find() && f.out->status == status::unreadable, "unmapped node is not not-found"); f.sanitized();
		cpu = static_cast<unsigned long>(vcpu::count()); check(!f.find(), "list CPU bounds"); cpu = 0;
		{
			list_fixture cycle(4); cycle.key(999); cycle.q->node_budget = 64;
			*cycle.head = {cycle.link(0), cycle.link(0)}; cycle.entry(0) = {cycle.head_va, cycle.head_va};
			cycle.entry(1) = {cycle.link(2), cycle.link(3)};
			cycle.entry(2) = {cycle.link(3), cycle.link(1)};
			cycle.entry(3) = {cycle.link(1), cycle.link(2)};
			cycle.q->resume_link = cycle.link(1); cycle.q->expected_prev = cycle.link(3);
			check(cycle.find() && cycle.out->status == status::inconsistent && cycle.out->visited == 3,
			      "detached reciprocal cycle rejected with constant space");
		}
	}

	void list_mutation_and_dispatch()
	{
		using status = hc::list_find_status;
		list_fixture f(1); f.key(1000);
		const auto data_hpa = gpa_hpa.at(*guest_leaf(f.source, f.objects[0]) & ~4095ULL);
		bool changed = false;
		std::function<void(std::uint64_t)> mutate;
		mutate = [&](std::uint64_t hpa) {
			if (hpa == data_hpa) { ++f.nodes[0][16]; changed = true; }
			else on_map = mutate;
		};
		on_map = mutate;
		check(f.find() && changed && f.out->status == status::inconsistent, "candidate key rechecked in returned window"); f.sanitized();
		f.nodes[0][16]--;
		mutate = [&](std::uint64_t hpa) {
			if (hpa == data_hpa) f.nodes[0][0] = 0xE1;
			else on_map = mutate;
		};
		on_map = mutate;
		check(f.find() && f.out->status == status::found && f.out->window[0] == 0xE1,
		      "found returns the final reread window");
		const auto request = *f.q;
		const auto head_hpa = gpa_hpa.at(*guest_leaf(f.source, f.head_va) & ~4095ULL);
		mutate = [&](std::uint64_t hpa) {
			if (hpa == head_hpa) { f.q->key[0] ^= 1; f.q->node_budget = 4097; }
			else on_map = mutate;
		};
		on_map = mutate;
		check(f.find() && f.out->status == status::found, "list request is snapshotted before traversal");
		*f.q = request;
		bool nested = true;
		on_map = [&](std::uint64_t) { nested = f.find(); };
		check(f.find() && !nested && f.out->status == status::found, "list reentry does not wait");
		check(!hc::list_find::find(f.source.flags, request_va, mask_va + 4096), "list publication failure invalidates result");
		check(!vcpu::curr().batch_work->busy.load(), "list publication failure releases work");
		guest_regs regs{}; regs.rcx = (hc::allowed_key << 16) | 12;
		regs.rdx = f.source.flags; regs.r8 = request_va; regs.r9 = mask_va;
		check(hc::handle(regs) && regs.rax == 1 && f.out->status == status::found, "operation 12 dispatch");
		regs.r10 = 1; check(hc::handle(regs) && !regs.rax, "operation 12 reserved R10");
		regs.r10 = 0; regs.r11 = 1; check(hc::handle(regs) && !regs.rax, "operation 12 reserved R11");
		// Source metadata can straddle two nonidentity physical pages.
		auto* second = map(caller, request_va + 4096);
		std::memcpy(reinterpret_cast<std::uint8_t*>(f.q) + 4090, &request, 6);
		std::memcpy(second, reinterpret_cast<const std::uint8_t*>(&request) + 6, sizeof(request) - 6);
		check(hc::list_find::find(f.source.flags, request_va + 4090, mask_va) && f.out->status == status::found,
		      "split list request");
	}

	void list_sdk()
	{
		hc::list_find_request q{}; q.version = 1; q.struct_size = sizeof(q);
		hc::list_find_result result{}; sdk_result = 1; sdk_calls = 0;
		check(hc::find_virtual_list(0x12345000, q, result) && sdk_calls == 1 && result.status == hc::list_find_status::more,
		      "list SDK single call");
		check(sdk_params.info.type == hc::type::virt_find_list && sdk_params.args[0] == 0x12345000 &&
		      sdk_params.args[1] == reinterpret_cast<std::uintptr_t>(&q) && sdk_params.args[2] == reinterpret_cast<std::uintptr_t>(&result) &&
		      !sdk_params.args[3] && !sdk_params.args[4], "list SDK arguments");
		sdk_result = 2;
		check(!hc::find_virtual_list(1, q, result) && std::all_of(reinterpret_cast<const std::uint8_t*>(&result),
		      reinterpret_cast<const std::uint8_t*>(&result) + sizeof(result), [](auto b) { return b == 0; }), "list SDK clears invalid response");
	}

	void list_split_and_object_bounds()
	{
		using status = hc::list_find_status;
		list_fixture f(1); f.key(1000);
		const auto object = source_va + 0x50000 + 4096 - 24;
		map(f.source, object & ~4095ULL); map(f.source, (object & ~4095ULL) + 4096);
		std::array<std::uint8_t, 128> bytes{};
		const std::uint64_t id = 1000;
		const list_fixture::pair pair{f.head_va, f.head_va};
		std::memcpy(bytes.data() + 16, &id, 8); std::memcpy(bytes.data() + 32, &pair, sizeof(pair));
		check(mem::guest::write_virt(f.source, object, bytes.data(), bytes.size()), "prepare split list object");
		*f.head = {object + 32, object + 32};
		check(f.find() && f.out->status == status::found && f.out->object_va == object &&
		      !std::memcmp(f.out->window, bytes.data(), bytes.size()), "split scan and returned window");
		map(caller, mask_va + 4096);
		const auto result_va = mask_va + 4090;
		check(hc::list_find::find(f.source.flags, request_va, result_va), "split list response publication");
		hc::list_find_result result{};
		check(mem::guest::read_virt(caller, result_va, &result, sizeof(result)) && result.status == status::found &&
		      !std::memcmp(result.window, bytes.data(), bytes.size()), "split list response complete");
		*guest_leaf(caller, mask_va + 4096) = 0;
		check(!hc::list_find::find(f.source.flags, request_va, result_va), "partial list publication returns failure");
		*guest_leaf(f.source, (object & ~4095ULL) + 4096) = 0;
		check(f.find() && f.out->status == status::unreadable, "split node short read"); f.sanitized();

		list_fixture invalid(0);
		constexpr std::uint64_t current = 0xFFFF800000000010ULL;
		auto* page = map(invalid.source, current & ~4095ULL);
		const list_fixture::pair node{invalid.head_va, invalid.head_va};
		std::memcpy(page + 16, &node, sizeof(node)); *invalid.head = {current, current};
		invalid.q->link_offset = invalid.q->key_offset = invalid.q->window_offset = 0xFFFFFFF0;
		invalid.q->window_size = 16; invalid.key(invalid.head_va);
		check(invalid.find() && invalid.out->status == status::inconsistent,
		      "reject noncanonical container even if field window is canonical");
	}

	void list_parallel()
	{
		list_fixture f(64); f.key(1063);
		struct control { std::uint64_t request, result, items, mask, data, dependent_items, dependent_result;
		                 hc::list_find_result* out; std::uint64_t* bits; hc::dependent_read_result* dependent; };
		std::array<control, 8> controls{};
		for (unsigned i = 0; i < controls.size(); ++i) {
			auto& c = controls[i]; c.request = request_va + 0x100000 + i * 0x10000;
			c.result = c.request + 4096; c.items = c.request + 8192; c.mask = c.request + 12288; c.data = c.request + 16384;
			std::memcpy(map(caller, c.request), f.q, sizeof(*f.q));
			c.out = reinterpret_cast<hc::list_find_result*>(map(caller, c.result));
			auto* item = reinterpret_cast<hc::batch_read_item*>(map(caller, c.items));
			*item = {f.objects[0] + 16, c.data, 8};
			c.bits = reinterpret_cast<std::uint64_t*>(map(caller, c.mask)); map(caller, c.data);
			c.dependent_items = c.request + 20480; c.dependent_result = c.request + 24576;
			auto* dependent = reinterpret_cast<hc::dependent_read_item*>(map(caller, c.dependent_items));
			dependent[0] = {f.head_va, 0, 8, hc::dependent_direct, hc::dependent_read_kind::pointer, 0};
			dependent[1] = {static_cast<std::uint64_t>(-16LL), c.data + 8, 8, 0, hc::dependent_read_kind::copy, 0};
			c.dependent = reinterpret_cast<hc::dependent_read_result*>(map(caller, c.dependent_result));
		}
		const auto root = caller; std::atomic<unsigned> failures = 0;
		std::vector<std::thread> threads;
		for (unsigned i = 0; i < controls.size(); ++i) threads.emplace_back([&, i] {
			caller = root; cpu = i; auto& c = controls[i];
			for (unsigned n = 0; n < 50; ++n) {
				if (!hc::list_find::find(f.source.flags, c.request, c.result) || c.out->status != hc::list_find_status::found ||
				    c.out->object_va != f.objects.back() || c.out->visited != 64) ++failures;
				if (!hc::batch::read(f.source.flags, c.items, 1, c.mask) || *c.bits != 1) ++failures;
				std::uint64_t id = 0;
				if (!hc::dependent::read(f.source.flags, c.dependent_items, 2, c.dependent_result) ||
				    c.dependent->status[1] != hc::dependent_read_status::success || c.dependent->pointers[0] != f.link(0) ||
				    !mem::guest::read_virt(caller, c.data + 8, &id, 8) || id != 1000) ++failures;
			}
		});
		for (auto& t : threads) t.join();
		check(failures == 0, "8 CPUs alternate operations 11/12/13 with shared scratch");
	}
}
