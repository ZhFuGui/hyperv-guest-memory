#pragma once

// Selected by the HypervisorArchitecture MSBuild property.
#if defined(ARCH_INTEL) && defined(ARCH_AMD)
	#error "Define only one of ARCH_INTEL or ARCH_AMD"
#endif

#if defined(ARCH_INTEL)
	#include "intel.hpp"
	using arch = intel;
#elif defined(ARCH_AMD)
	#include "amd.hpp"
	using arch = amd;
#else
	#error "Define ARCH_INTEL or ARCH_AMD"
#endif
