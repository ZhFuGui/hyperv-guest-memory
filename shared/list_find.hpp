#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace hc
{
	inline constexpr std::uint32_t list_find_version = 1;
	inline constexpr std::uint32_t list_find_default_nodes = 1024;
	inline constexpr std::uint32_t list_find_max_nodes = 4096;
	inline constexpr std::uint32_t list_find_max_window = 1024;
	inline constexpr std::uint32_t list_find_max_key = 16;

	enum class list_find_status : std::uint32_t
	{
		found = 1, not_found = 2, more = 3, unreadable = 4, inconsistent = 5
	};

	// Initialize version=1 and struct_size=sizeof(list_find_request).
	// The window must contain the 16-byte link and the entire key field.
	struct list_find_request
	{
		std::uint32_t version, struct_size;
		std::uint64_t head_va; // Sentinel LIST_ENTRY, not an object base.
		std::uint64_t resume_link, expected_prev; // First call: both 0; more: copy both cursor fields.
		std::uint32_t link_offset, window_offset, window_size, key_offset, key_size;
		std::uint32_t node_budget; // 0 selects default; otherwise 1..4096.
		std::uint8_t key[list_find_max_key];
	};

	struct list_find_result
	{
		list_find_status status;
		std::uint32_t visited; // Successfully scanned/locally validated nodes.
		std::uint64_t object_va; // found only.
		std::uint64_t next_link, previous_link; // more only; invalidatable cursor.
		std::uint32_t window_size, reserved;
		std::uint8_t window[list_find_max_window]; // found only; unused bytes zero.
	};

	// Results are observations, not an atomic snapshot or an object reference.
	// not_found means this traversal reached the sentinel without observing a hit.
	// Resume using the same CR3, head, offsets and key. It proves local linkage
	// only, not reachability or identity across calls. Callers bound total
	// continuation/retry work, keep controls resident/writable
	// as applicable, and must not alias controls with the source list.
	static_assert(std::is_trivially_copyable_v<list_find_request> && std::is_standard_layout_v<list_find_request>);
	static_assert(std::is_trivially_copyable_v<list_find_result> && std::is_standard_layout_v<list_find_result>);
	static_assert(sizeof(list_find_request) == 72 && alignof(list_find_request) == 8);
	static_assert(offsetof(list_find_request, head_va) == 8 && offsetof(list_find_request, key) == 56);
	static_assert(sizeof(list_find_result) == 1064 && alignof(list_find_result) == 8);
	static_assert(offsetof(list_find_result, object_va) == 8 && offsetof(list_find_result, window) == 40);
}
