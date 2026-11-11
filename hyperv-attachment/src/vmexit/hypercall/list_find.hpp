#pragma once
#include <cstdint>
namespace hc::list_find
{
	bool find(std::uint64_t source_cr3, std::uintptr_t request_va, std::uintptr_t result_va) noexcept;
}
