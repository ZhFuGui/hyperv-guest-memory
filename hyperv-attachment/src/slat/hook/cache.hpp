#pragma once
#include <cstdint>
#include <cstddef>

namespace slat::cache
{
	bool handle() noexcept;
	std::uint64_t flush_all(bool notify = true) noexcept;
	std::uint64_t epoch() noexcept;
	std::size_t pending(std::uint64_t target) noexcept;
}
