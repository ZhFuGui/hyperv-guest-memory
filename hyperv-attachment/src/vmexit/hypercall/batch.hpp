#pragma once
#include <batch.hpp>
#include <atomic.hpp>

namespace hc::batch
{
	struct workspace
	{
		cstd::atomic<bool> busy{};
		batch_read_item items[batch_max_items]{};
	};

	bool read(std::uint64_t source_cr3, std::uintptr_t items_va,
	          std::uint64_t count, std::uintptr_t mask_va) noexcept;
}
