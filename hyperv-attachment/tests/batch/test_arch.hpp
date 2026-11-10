#pragma once
#include <ia32.hpp>
#include "../../src/arch/defs.hpp"
namespace test
{
	extern thread_local cr3 caller;
	extern cr3 slat;
}
struct arch
{
	static cr3 guest_cr3() { return test::caller; }
	static cr3 slat_cr3() { return test::slat; }
};
