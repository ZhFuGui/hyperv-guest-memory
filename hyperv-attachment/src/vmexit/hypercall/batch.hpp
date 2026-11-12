#pragma once
#include <batch.hpp>
#include <dependent.hpp>
#include <atomic.hpp>

namespace hc::batch
{
	struct workspace
	{
		// The gate also provides natural alignment for the shared scratch arena.
		cstd::atomic<std::uint64_t> busy{};
		// Shared by synchronous read/find operations, with explicit POD lifetimes.
		alignas(8) std::uint8_t storage[sizeof(dependent_read_item) * dependent_max_items + sizeof(dependent_read_result)]{};
	};
	static_assert(sizeof(workspace) <= 4096); // At most one preallocated page per CPU.

	bool read(std::uint64_t source_cr3, std::uintptr_t items_va,
	          std::uint64_t count, std::uintptr_t mask_va) noexcept;
}
