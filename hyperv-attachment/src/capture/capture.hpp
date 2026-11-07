#pragma once
#include "../arch/current.hpp"
#include <capture.hpp>

namespace capture
{
	bool push(std::uint64_t cr3, std::uint64_t va, std::uint64_t gpa,
	          std::uint64_t rip, std::uint32_t why, const void* data) noexcept;

	std::size_t flush(const cr3 dest_cr3, std::uintptr_t dest_va, std::uint16_t count) noexcept;
	std::size_t count() noexcept;
	std::size_t captured_bytes() noexcept;
	std::uint64_t dropped() noexcept;
	bool reset() noexcept;
	void note_drop() noexcept;
}
