#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace hc
{
	inline constexpr std::uint32_t dependent_max_items = 64;
	inline constexpr std::uint64_t dependent_max_bytes = 64 * 1024;
	inline constexpr std::uint16_t dependent_direct = UINT16_MAX;

	enum class dependent_read_kind : std::uint8_t { pointer = 1, copy = 2 };
	enum class dependent_read_status : std::uint8_t
	{
		unused = 0, success = 1, invalid_address = 2, transfer_failed = 3, dependency_failed = 4
	};

	struct dependent_read_item
	{
		// Absolute VA if parent==dependent_direct; otherwise a signed byte offset
		// encoded in two's complement (convert negative int64_t offsets to uint64_t).
		std::uint64_t address;
		std::uint64_t destination_va; // copy only; pointer requires 0.
		std::uint32_t size; // pointer requires 8; copy requires 1..dependent_max_bytes.
		std::uint16_t parent; // dependent_direct or an earlier pointer item's index.
		dependent_read_kind kind;
		std::uint8_t reserved; // Must be 0.
	};

	struct dependent_read_result
	{
		std::uint64_t pointers[dependent_max_items]; // Successful pointer items only.
		dependent_read_status status[dependent_max_items]; // Unused entries are zero.
	};

	// Synchronous, read-only observation in one source CR3, not an atomic snapshot
	// or an object reference. Every pointer read counts towards the 64 KiB budget.
	// Null/noncanonical pointer values fail with invalid_address; descendants are
	// skipped, independent items continue. transfer_failed covers either endpoint
	// of a copy. Only success permits use of that item's output; a failed copy may
	// have partially overwritten its destination. RAX!=1 invalidates ALL outputs.
	// Controls/outputs must be in the lower canonical user half. Each guest page
	// walk requires U for controls, and W|U for outputs, at every paging level.
	// Output writes also require a writable SLAT mapping.
	// Callers keep controls/outputs resident, exclusively owned and writable where
	// applicable. Controls and nonempty outputs must not overlap; do not provide
	// physical aliases of controls/outputs or alias them with the source data.
	static_assert(std::is_standard_layout_v<dependent_read_item> && std::is_trivially_copyable_v<dependent_read_item>);
	static_assert(sizeof(dependent_read_item) == 24 && alignof(dependent_read_item) == 8);
	static_assert(offsetof(dependent_read_item, destination_va) == 8 && offsetof(dependent_read_item, size) == 16);
	static_assert(offsetof(dependent_read_item, parent) == 20 && offsetof(dependent_read_item, kind) == 22 &&
	              offsetof(dependent_read_item, reserved) == 23);
	static_assert(std::is_standard_layout_v<dependent_read_result> && std::is_trivially_copyable_v<dependent_read_result>);
	static_assert(sizeof(dependent_read_result) == 576 && alignof(dependent_read_result) == 8);
	static_assert(offsetof(dependent_read_result, pointers) == 0 && offsetof(dependent_read_result, status) == 512);
}
