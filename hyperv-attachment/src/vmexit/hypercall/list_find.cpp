#include "list_find.hpp"
#include "batch.hpp"
#include "../../mem/guest.hpp"
#include "../../vcpu/vcpu.hpp"
#include <list_find.hpp>

namespace
{
	using status = hc::list_find_status;
	struct links { std::uint64_t next, prev; };
	struct state { hc::list_find_request request; hc::list_find_result result; };
	static_assert(sizeof(state) <= sizeof(hc::batch::workspace::storage));

	bool range(std::uint64_t va, std::uint64_t size) noexcept
	{
		if (!va || !size || size - 1 > UINT64_MAX - va) return false;
		const auto top = va >> 47;
		return (top == 0 || top == 0x1FFFF) && ((va + size - 1) >> 47) == top;
	}
	bool link_address(std::uint64_t va) noexcept { return !(va & 7) && range(va, sizeof(links)); }
	bool address(std::uint64_t base, std::uint64_t offset, std::uint64_t size, std::uint64_t& va) noexcept
	{
		if (offset > UINT64_MAX - base) return false;
		va = base + offset;
		return range(va, size);
	}
	bool equal(const void* a, const void* b, std::size_t size) noexcept
	{
		const auto* x = static_cast<const std::uint8_t*>(a);
		const auto* y = static_cast<const std::uint8_t*>(b);
		for (std::size_t i = 0; i < size; ++i) if (x[i] != y[i]) return false;
		return true;
	}
	struct reader
	{
		cr3 source, slat;
		bool read(std::uint64_t va, void* out, std::size_t size) const noexcept
		{
			return range(va, size) && mem::guest::read_virt(source, slat, va, out, size);
		}
		status validate(std::uint64_t current, std::uint64_t previous, const links& item) const noexcept
		{
			if (!link_address(item.next) || !link_address(item.prev) || item.prev != previous ||
			    item.next == current || item.prev == current) return status::inconsistent;
			std::uint64_t forward = 0, backward = 0;
			links again{};
			if (!read(previous, &forward, 8) || !read(item.next + 8, &backward, 8) ||
			    !read(current, &again, sizeof(again))) return status::unreadable;
			return forward == current && backward == current && equal(&item, &again, sizeof(item))
				? status::found : status::inconsistent;
		}
	};

	void search(const reader& memory, const hc::list_find_request& q, hc::list_find_result& out) noexcept
	{
		out.status = status::unreadable;
		links head{};
		if (!memory.read(q.head_va, &head, sizeof(head))) return;
		if (!link_address(head.next) || !link_address(head.prev) ||
		    ((head.next == q.head_va) != (head.prev == q.head_va))) { out.status = status::inconsistent; return; }
		if (head.next == q.head_va) {
			out.status = q.resume_link ? status::inconsistent : status::not_found; return;
		}
		auto current = q.resume_link ? q.resume_link : head.next;
		auto previous = q.resume_link ? q.expected_prev : q.head_va;
		if (q.resume_link) {
			links resume{};
			if (!memory.read(current, &resume, sizeof(resume))) return;
			out.status = memory.validate(current, previous, resume);
			if (out.status != status::found) return;
		}
		const auto first = current;
		const std::uint64_t start = q.link_offset < q.key_offset ? q.link_offset : q.key_offset;
		const std::uint64_t link_end = std::uint64_t(q.link_offset) + sizeof(links);
		const std::uint64_t key_end = std::uint64_t(q.key_offset) + q.key_size;
		const auto scan_size = static_cast<std::size_t>((link_end > key_end ? link_end : key_end) - start);
		const auto budget = q.node_budget ? q.node_budget : hc::list_find_default_nodes;
		for (std::uint32_t n = 0; n < budget; ++n) {
			std::uint64_t scan_va = 0, window_va = 0;
			if (!link_address(current) || current < q.link_offset ||
			    !range(current - q.link_offset, 1) ||
			    !address(current - q.link_offset, start, scan_size, scan_va) ||
			    !address(current - q.link_offset, q.window_offset, q.window_size, window_va)) {
				out.status = status::inconsistent; return;
			}
			out.status = status::unreadable;
			if (!memory.read(scan_va, out.window, scan_size)) return;
			links item{};
			cstd::crt::memcpy(&item, out.window + (q.link_offset - start), sizeof(item));
			out.status = memory.validate(current, previous, item);
			if (out.status != status::found) return;
			++out.visited;
			if (equal(out.window + (q.key_offset - start), q.key, q.key_size)) {
				out.status = status::unreadable;
				if (!memory.read(window_va, out.window, q.window_size)) return;
				links final{};
				cstd::crt::memcpy(&final, out.window + (q.link_offset - q.window_offset), sizeof(final));
				if (!equal(&item, &final, sizeof(item)) ||
				    !equal(out.window + (q.key_offset - q.window_offset), q.key, q.key_size)) {
					out.status = status::inconsistent; return;
				}
				out.status = memory.validate(current, previous, final);
				if (out.status == status::found) {
					out.object_va = current - q.link_offset; out.window_size = q.window_size;
				}
				return;
			}
			previous = current; current = item.next;
			if (current == q.head_va) {
				links end{}; out.status = status::unreadable;
				if (!memory.read(q.head_va, &end, sizeof(end))) return;
				out.status = end.prev == previous && end.next == head.next ? status::not_found : status::inconsistent;
				return;
			}
			if (current == first) { out.status = status::inconsistent; return; }
		}
		out.status = status::more; out.next_link = current; out.previous_link = previous;
	}
}

bool hc::list_find::find(std::uint64_t source_cr3, std::uintptr_t request_va, std::uintptr_t result_va) noexcept
{
	if (!(source_cr3 & 0x0000FFFFFFFFF000ULL) || (source_cr3 >> 48) ||
	    !range(request_va, sizeof(list_find_request)) || !range(result_va, sizeof(list_find_result)) ||
	    (request_va <= result_va ? result_va - request_va < sizeof(list_find_request)
	                             : request_va - result_va < sizeof(list_find_result))) return false;
	const auto cpu = vcpu::curr_id();
	if (cpu >= vcpu::count()) return false;
	auto* const work = vcpu::get(cpu).batch_work;
	if (!work || work->busy.exchange(true, cstd::memory_order::acquire)) return false;
	struct release { hc::batch::workspace& w; ~release() { w.busy.store(false, cstd::memory_order::release); } } done{*work};
	const auto caller = arch::guest_cr3(), slat = arch::slat_cr3();
	if (!caller.address_of_page_directory || !slat.address_of_page_directory) return false;
	auto* const s = new(work->storage) state{};
	if (!mem::guest::read_virt(caller, slat, request_va, &s->request, sizeof(s->request))) return false;
	const auto& q = s->request;
	const std::uint64_t end = std::uint64_t(q.window_offset) + q.window_size;
	if (q.version != list_find_version || q.struct_size != sizeof(q) || !link_address(q.head_va) ||
	    !q.window_size || q.window_size > list_find_max_window || !q.key_size || q.key_size > list_find_max_key ||
	    q.node_budget > list_find_max_nodes || q.window_offset > q.link_offset || q.window_offset > q.key_offset ||
	    end < std::uint64_t(q.link_offset) + sizeof(links) || end < std::uint64_t(q.key_offset) + q.key_size ||
	    (!q.resume_link && q.expected_prev) || (q.resume_link && (!link_address(q.resume_link) ||
	     !link_address(q.expected_prev) || q.resume_link == q.head_va || q.resume_link == q.expected_prev))) return false;
	search(reader{cr3{.flags = source_cr3}, slat}, q, s->result);
	auto& out = s->result;
	if (out.status != status::found) {
		out.object_va = 0; out.window_size = 0;
		cstd::crt::memset(out.window, 0, sizeof(out.window));
	} else cstd::crt::memset(out.window + out.window_size, 0, sizeof(out.window) - out.window_size);
	if (out.status != status::more) out.next_link = out.previous_link = 0;
	return mem::guest::write_virt(caller, slat, result_va, &out, sizeof(out));
}
