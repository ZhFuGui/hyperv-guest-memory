#pragma once
#include <batch.hpp>
#include <atomic.hpp>

namespace hc::batch
{
	struct workspace
	{
		// The gate also provides natural alignment for the shared scratch arena.
		cstd::atomic<std::uint64_t> busy{};
		// Shared by synchronous read/find operations, with explicit POD lifetimes.
		alignas(8) std::uint8_t storage[sizeof(batch_read_item) * batch_max_items]{};
	};

	bool read(std::uint64_t source_cr3, std::uintptr_t items_va,
	          std::uint64_t count, std::uintptr_t mask_va) noexcept;
}
