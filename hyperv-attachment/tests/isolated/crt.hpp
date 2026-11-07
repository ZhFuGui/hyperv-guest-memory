#pragma once
#include <cstddef>
#include <cstdint>
namespace cstd::crt
{
	void memcpy(void*, const void*, std::size_t);
	void memset(void*, std::int32_t, std::size_t);
}
