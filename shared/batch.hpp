#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace hc
{
	inline constexpr std::uint32_t batch_max_items = 64;
	inline constexpr std::uint64_t batch_max_bytes = 64 * 1024;

	// Synchronous read-only gather. Destinations belong to the calling address
	// space. A set result bit means the ENTIRE item was copied; otherwise its
	// destination is invalid and may have been partially overwritten.
	// Control buffers and nonempty destinations must not overlap. Callers own
	// the buffers exclusively for the duration of the call and keep them resident
	// (destinations and mask writable). The range checks are virtual; callers must
	// not supply physical aliases of controls or other outputs. This is not an
	// atomic snapshot of the source. A zero-size item succeeds without access.
	struct batch_read_item
	{
		std::uint64_t source_va;
		std::uint64_t destination_va;
		std::uint64_t size;
	};

	static_assert(std::is_standard_layout_v<batch_read_item>);
	static_assert(std::is_trivially_copyable_v<batch_read_item>);
	static_assert(sizeof(batch_read_item) == 24 && alignof(batch_read_item) == 8);
	static_assert(offsetof(batch_read_item, source_va) == 0);
	static_assert(offsetof(batch_read_item, destination_va) == 8);
	static_assert(offsetof(batch_read_item, size) == 16);
}
