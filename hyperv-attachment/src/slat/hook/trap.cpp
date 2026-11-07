#include "trap.hpp"
#include "info.hpp"
#include "cache.hpp"
#include "../cr3/cr3.hpp"
#include "../cr3/split.hpp"
#include "../../arch/current.hpp"
#include "../../capture/capture.hpp"
#include "../../mem/mem.hpp"
#include "../../vcpu/vcpu.hpp"
#include <atomic>

namespace
{
	constexpr std::size_t table_size = 8192;
	constexpr std::size_t table_mask = table_size - 1;
	constexpr std::uint64_t empty_key = 0, deleted_key = 1;
	enum class phase : std::uint32_t { arming, armed, copying, captured, lost, retiring, retired, error };
	struct entry
	{
		std::atomic<std::uint64_t> key{};
		std::atomic<phase> state{phase::retired};
		// Immutable between publishing key and a reader grace period after unpublishing it.
		arch::slat_pte* pte = nullptr;
		std::uint64_t original = 0;
		std::uint64_t root = 0;
		std::uint64_t retired_epoch = 0; // control writer only
	};
	constinit entry table[table_size];
	constinit std::atomic<std::uint32_t> readers{};
	constinit std::atomic<std::uint64_t> lost{}, restore_failures{}, unhandled{};
	constinit std::atomic<std::uint64_t> last_gpa{}, last_rip{}, last_qualification{}, last_root{}, sample_version{};
	constinit std::atomic_flag sampling = ATOMIC_FLAG_INIT;
	static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free);

	struct read_guard
	{
		read_guard() { readers.fetch_add(1); }
		~read_guard() { readers.fetch_sub(1); }
	};

	std::uint64_t key_for(const phys_addr page) noexcept { return (page >> page_shift) + 2; }
	phys_addr page_for(const std::uint64_t key) noexcept { return (key - 2) << page_shift; }

	entry* find(const phys_addr page) noexcept
	{
		const auto key = key_for(page);
		for (std::size_t n = 0; n < table_size; ++n)
		{
			auto& item = table[((page >> page_shift) + n) & table_mask];
			const auto seen = item.key.load();
			if (seen == empty_key) return nullptr;
			if (seen == key) return &item;
		}
		return nullptr;
	}

	void reclaim_retired() noexcept
	{
		// A remote CPU must have invalidated old permissions before a key is removed.
		if (slat::cache::pending(slat::cache::epoch())) return;
		for (auto& item : table)
			if (item.key.load() > deleted_key && item.state.load() == phase::retired && item.retired_epoch)
				item.key.store(deleted_key);
		// Metadata in deleted slots is reused only after readers == 0 (see reserve).
	}

	entry* reserve(const phys_addr page) noexcept
	{
		for (std::size_t n = 0; n < table_size; ++n)
		{
			auto& item = table[((page >> page_shift) + n) & table_mask];
			const auto key = item.key.load();
			if (key == empty_key || (key == deleted_key && readers.load() == 0))
				return &item;
		}
		return nullptr;
	}

	bool set_execute(entry& item, const phys_addr page, const bool enable) noexcept
	{
		const ::cr3 root{.flags = item.root};
		if (slat::cr3::find_leaf(root, page) != item.pte) return false;
		std::atomic_ref<std::uint64_t> flags(item.pte->flags);
		auto current = flags.load(std::memory_order_acquire);
		constexpr auto identity = ~(arch::slat_execute_mask | arch::slat_ad_mask);
		for (unsigned retry = 0; retry < 8; ++retry)
		{
			if ((current & identity) != (item.original & identity)) return false;
			const auto bits = enable ? (item.original & arch::slat_execute_mask) : arch::slat_nx_bits;
			const auto desired = (current & ~arch::slat_execute_mask) | bits;
			if (desired == current || flags.compare_exchange_strong(current, desired)) return true;
		}
		return false;
	}

	bool restore(entry& item, const phys_addr page, const std::uint64_t mask = 0, const bool invalidate = true) noexcept
	{
		if (!set_execute(item, page, true) || !slat::cr3::allows_execute(::cr3{.flags = item.root}, page, mask))
		{
			restore_failures.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		if (invalidate) arch::flush_slat_cache();
		return true;
	}

	bool retire(entry& item, const phys_addr page) noexcept
	{
		item.state.exchange(phase::retiring);
		if (!restore(item, page, 0, false))
		{
			item.state.store(phase::error);
			return false;
		}
		item.state.store(phase::retired);
		return true;
	}
}

bool slat::hook::trap_exec(const phys_addr gpa, const bool notify) noexcept
{
	const auto page = gpa & ~page_mask;
	if (!cr3::hyperv.flags || page >= (1ull << 48) || vcpu::count() > capture::max_cpus) return false;
	cstd::unique_lock control(mtx, cstd::try_to_lock);
	if (!control.owns_lock() || !list.empty() || shadow_owner.load(std::memory_order_acquire) != UINT64_MAX) return false;
	reclaim_retired();
	auto* item = find(page);
	if (item)
	{
		const auto state = item->state.load();
		if (state == phase::arming || state == phase::armed || state == phase::copying || state == phase::captured)
			return true;
		if (state != phase::lost && state != phase::retired) return false;
		if (state == phase::retired && cache::pending(item->retired_epoch)) return false;
		// Unpublish before the grace check: a new reader must not enter the old generation.
		item->key.store(deleted_key);
		if (readers.load()) return false;
	}
	if (!item)
	{
		item = reserve(page);
		if (!item) return false;
	}
	{
		auto* pte = cr3::find_split_leaf(cr3::hyperv, page);
		if (!pte) return false;
		const auto original = std::atomic_ref<std::uint64_t>(pte->flags).load();
		const arch::slat_pte snapshot{.flags = original};
		if (!arch::slat_is_present(snapshot) || !cr3::allows_execute(cr3::hyperv, page))
			return false;
		item->pte = pte;
		item->original = original;
		item->root = cr3::hyperv.flags;
		item->retired_epoch = 0;
		item->state.store(phase::arming);
		item->key.store(key_for(page)); // publish ownership BEFORE removing execute permission
	}

	if (!set_execute(*item, page, false))
	{
		item->state.store(phase::error);
		return false;
	}
	auto expected = phase::arming;
	if (!item->state.compare_exchange_strong(expected, phase::armed))
	{
		// An early fault/clear cancelled the arm. Never leave NX behind after it restored X.
		if (!restore(*item, page)) { item->state.store(phase::error); return false; }
	}
	cache::flush_all(notify);
	return true;
}

bool slat::hook::trap_capture(const phys_addr gpa, const std::uint64_t execute_mask) noexcept
{
	read_guard guard;
	const auto page = gpa & ~page_mask;
	auto* item = find(page);
	if (!item) return false;
	const auto required = execute_mask ? execute_mask : arch::slat_active_execute_mask();
	if (!((item->original ^ arch::slat_nx_bits) & required)) return false;
	auto state = item->state.load();
	bool owner = false;
	if (state == phase::arming || state == phase::armed)
		owner = item->state.compare_exchange_strong(state, phase::copying);
	if (!owner) return restore(*item, page, execute_mask);

	const auto hpa = (item->original >> page_shift) & ((1ull << 40) - 1);
	const auto* src = mem::map_phys<const std::uint8_t*>(hpa << page_shift);
	const auto rip = arch::rip();
	const bool saved = capture::push(arch::guest_cr3().flags, rip & ~page_mask,
		page, rip, static_cast<std::uint32_t>(capture::reason::exec), src);
	if (!saved) lost.fetch_add(1, std::memory_order_relaxed);
	const bool restored = restore(*item, page, execute_mask);
	auto expected = phase::copying;
	item->state.compare_exchange_strong(expected, restored ? (saved ? phase::captured : phase::lost) : phase::error);
	return restored;
}

bool slat::hook::trap_remove(const phys_addr gpa) noexcept
{
	cstd::unique_lock control(mtx, cstd::try_to_lock);
	if (!control.owns_lock()) return false;
	auto* item = find(gpa & ~page_mask);
	if (!item) return false;
	const bool restored = retire(*item, gpa & ~page_mask);
	const auto epoch = cache::flush_all();
	if (restored) item->retired_epoch = epoch;
	return restored;
}

bool slat::hook::trap_clear_all() noexcept
{
	cstd::unique_lock control(mtx, cstd::try_to_lock);
	if (!control.owns_lock()) return false;
	bool attempted = false;
	for (auto& item : table)
	{
		const auto key = item.key.load();
		if (key <= deleted_key || item.state.load() == phase::retired) continue;
		attempted = true;
		retire(item, page_for(key));
	}
	if (attempted)
	{
		const auto epoch = cache::flush_all();
		for (auto& item : table)
			if (item.key.load() > deleted_key && item.state.load() == phase::retired)
				item.retired_epoch = epoch;
	}
	reclaim_retired();
	return true;
}

std::size_t slat::hook::trap_count() noexcept
{
	read_guard guard;
	std::size_t count = 0;
	for (const auto& item : table)
		if (item.key.load() > deleted_key && item.state.load() != phase::retired) ++count;
	return count;
}

std::size_t slat::hook::trap_captured() noexcept
{
	read_guard guard;
	std::size_t count = 0;
	for (const auto& item : table)
		if (item.key.load() > deleted_key && item.state.load() == phase::captured) ++count;
	return count;
}

bool slat::hook::trap_blocks_hooks() noexcept
{
	reclaim_retired();
	if (readers.load()) return true;
	for (const auto& item : table)
		if (item.key.load() > deleted_key) return true;
	return false;
}

void slat::hook::trap_note_unhandled(const slat_violation& fault) noexcept
{
	unhandled.fetch_add(1, std::memory_order_relaxed);
	if (sampling.test_and_set(std::memory_order_acquire)) return;
	sample_version.fetch_add(1);
	last_gpa.store(fault.gpa);
	last_rip.store(arch::rip());
	last_qualification.store(fault.qualification);
	last_root.store(arch::slat_cr3().flags);
	sample_version.fetch_add(1);
	sampling.clear(std::memory_order_release);
}

capture::trap_status slat::hook::trap_stats() noexcept
{
	const auto epoch = cache::epoch();
	capture::trap_status result{};
	result.registered = trap_count();
	result.captured = trap_captured();
	result.lost = lost.load();
	result.restore_failures = restore_failures.load();
	result.unhandled = unhandled.load();
	const auto version = sample_version.load();
	result.last_gpa = last_gpa.load();
	result.last_rip = last_rip.load();
	result.last_qualification = last_qualification.load();
	result.last_root = last_root.load();
	if ((version & 1) || version != sample_version.load())
		result.last_gpa = result.last_rip = result.last_qualification = result.last_root = 0;
	result.flush_epoch = epoch;
	result.pending_cpus = cache::pending(epoch);
	return result;
}
