#pragma once
#include <cstdint>
#include <cstddef>

// Shared between the hypervisor payload (producer) and usermode (consumer).
// Layout must stay POD and identical on both sides.
namespace capture
{
	// Per-CPU rings: 64 records each. On a 16-vCPU machine that is 1024 records
	// (~4.2 MB) of buffering. Each ring has one producer and one consumer.
	inline constexpr std::size_t per_cpu_capacity = 64;
	inline constexpr std::size_t max_cpus = 32;
	inline constexpr std::size_t total_capacity = per_cpu_capacity * max_cpus;
	inline constexpr std::size_t page_bytes = 4096;
	inline constexpr std::size_t max_flush_records = 16;

	struct trap_status
	{
		std::uint64_t registered;
		std::uint64_t captured;
		std::uint64_t lost;
		std::uint64_t restore_failures;
		std::uint64_t unhandled;
		std::uint64_t last_gpa;
		std::uint64_t last_rip;
		std::uint64_t last_qualification;
		std::uint64_t last_root;
		std::uint64_t flush_epoch;
		std::uint64_t pending_cpus;
	};

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
