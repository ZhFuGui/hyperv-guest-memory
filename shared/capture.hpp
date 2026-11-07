#pragma once
#include <cstdint>
#include <cstddef>

// Shared between the hypervisor payload (producer) and usermode (consumer).
// Layout must stay POD and identical on both sides.
namespace capture
{
	inline constexpr std::size_t capacity = 128;
	inline constexpr std::size_t page_bytes = 4096;

	enum class reason : std::uint32_t
	{
		exec = 0,
		write = 1,
		new_page = 2,
	};

#pragma pack(push, 1)
	struct rec
	{
		std::uint64_t seq;
		std::uint64_t cr3;
		std::uint64_t va;
		std::uint64_t gpa;
		std::uint64_t rip;
		std::uint32_t why;
		std::uint32_t flags;
		std::uint8_t data[page_bytes];
	};
#pragma pack(pop)

	static_assert(sizeof(rec) == 4144, "capture record layout changed");
}
