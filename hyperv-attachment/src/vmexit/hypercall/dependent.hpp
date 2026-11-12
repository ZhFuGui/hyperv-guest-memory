#pragma once
#include <cstdint>

namespace hc::dependent
{
	bool read(std::uint64_t source_cr3, std::uintptr_t items_va,
	          std::uint64_t count, std::uintptr_t result_va) noexcept;
}
