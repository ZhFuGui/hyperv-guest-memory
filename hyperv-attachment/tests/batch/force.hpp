#pragma once
#include <intrin.h>
// Only replace hardware context and the identity mapping. The batch handler,
// guest/SLAT page walks, copy loops, dispatcher, vCPU lifecycle and SDK are real.
#pragma include_alias("../arch/current.hpp", "test_arch.hpp")
#pragma include_alias("../../arch/current.hpp", "test_arch.hpp")
#pragma include_alias("map.hpp", "test_map.hpp")
unsigned long batch_test_cpu_id() noexcept;
#define __readgsdword(offset) batch_test_cpu_id()
