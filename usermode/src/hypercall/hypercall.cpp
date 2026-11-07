#include "hypercall.hpp"

extern "C" std::uint64_t send_hypercall(const hc::params* params);

namespace
{
	std::uint64_t send(const hc::type t,
	                   std::uint64_t a0 = 0, std::uint64_t a1 = 0,
	                   std::uint64_t a2 = 0, std::uint64_t a3 = 0,
	                   std::uint64_t a4 = 0) noexcept
	{
		hc::params p{};
		p.info.key = hc::allowed_key;
		p.info.type = t;
		p.args[0] = a0;
		p.args[1] = a1;
		p.args[2] = a2;
		p.args[3] = a3;
		p.args[4] = a4;

		return send_hypercall(&p);
	}
}

std::uint64_t hc::test() noexcept
{
	return send(type::test);
}

std::uint64_t hc::curr_cr3() noexcept
{
	return send(type::curr_cr3);
}

bool hc::phys_copy(const std::uintptr_t src_phys, const std::uintptr_t dest_phys,
                    const std::uint64_t size) noexcept
{
	return send(type::phys_mem_op, src_phys, dest_phys, size);
}

bool hc::virt_copy(const std::uint64_t src_cr3, const std::uintptr_t src_va,
                    const std::uint64_t dest_cr3, const std::uintptr_t dest_va,
                    const std::uint64_t size) noexcept
{
	return send(type::virt_mem_op, src_cr3, src_va, dest_cr3, dest_va, size);
}

std::uintptr_t hc::translate_va(const std::uint64_t cr3, const std::uintptr_t va) noexcept
{
	return send(type::translate_va, cr3, va);
}

bool hc::hook_add(const std::uintptr_t target_gpa, const std::uintptr_t shadow_gpa) noexcept
{
	return send(type::hook_add, target_gpa, shadow_gpa);
}

bool hc::hook_remove(const std::uintptr_t target_gpa) noexcept
{
	return send(type::hook_remove, target_gpa);
}

bool hc::hide_page(const std::uintptr_t target_gpa) noexcept
{
	return send(type::hide_page, target_gpa);
}

std::size_t hc::rem_heap_size() noexcept
{
	return send(type::rem_heap_size);
}

bool hc::add_log() noexcept
{
	return send(type::add_log);
}

std::size_t hc::flush_logs(logs::entry* const out, const std::uint16_t count) noexcept
{
	return send(type::flush_logs, reinterpret_cast<std::uintptr_t>(out), count);
}

bool hc::trap_exec(const std::uintptr_t gpa, const bool defer_sync) noexcept
{
	return send(type::trap_exec, gpa, defer_sync ? 1 : 0);
}

std::size_t hc::trap_clear_all() noexcept
{
	return send(type::trap_clear_all);
}

std::uint64_t hc::trap_sync() noexcept { return send(type::trap_sync); }

bool hc::trap_stats(capture::trap_status* const out) noexcept
{
	return out && send(type::trap_stats, reinterpret_cast<std::uintptr_t>(out));
}

std::size_t hc::capture_flush(void* const out, const std::uint16_t count) noexcept
{
	return send(type::capture_flush, curr_cr3(), reinterpret_cast<std::uintptr_t>(out), count);
}

std::uint64_t hc::capture_stats() noexcept
{
	return send(type::capture_stats);
}
