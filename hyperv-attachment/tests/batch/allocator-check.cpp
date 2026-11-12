// Execute the REAL slab/page allocators against aligned private host RAM.
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>
#include <boot_info.hpp>
#include <heap.hpp>
#include <identity_map.hpp>
#include "../../src/mem/alloc/slab.hpp"
#include "../../src/mem/alloc/page.hpp"
#include "../../src/vmexit/hypercall/batch.hpp"

namespace
{
	alignas(4096) std::array<unsigned char, hva_heap_page_count * 4096> arena{};
	void check(bool value, const char* message)
	{
		if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
	}
}
namespace cstd::crt
{
	void memcpy(void* dst, const void* src, std::size_t size) { std::memcpy(dst, src, size); }
	void memset(void* dst, std::int32_t value, std::size_t size) { std::memset(dst, value, size); }
}

int main()
{
	boot_info bi{};
	// Rebase the boot heap onto private host RAM; unsigned arithmetic is modulo.
	bi.heap_range.base = reinterpret_cast<std::uintptr_t>(arena.data()) - hva_identity_map_base();
	bi.heap_range.size = arena.size(); bi.usable_heap_base = bi.heap_range.base + 16 * 4096;
	mem::page::init(bi);
	const auto initial = mem::page::free_count();
	check(initial == hva_heap_page_count - 16, "reserved boot pages");
	using workspace = hc::batch::workspace;
	static_assert(sizeof(workspace) > 2048 && sizeof(workspace) <= 4096);
	std::array<workspace*, 16> work{};
	for (unsigned i = 0; i < work.size(); ++i) {
		void* const memory = mem::slab::alloc(sizeof(workspace));
		check(memory && !(reinterpret_cast<std::uintptr_t>(memory) & 4095), "one-page workspace allocation");
		work[i] = new(memory) workspace{};
		check(!work[i]->busy.load(), "workspace gate initialized");
		std::memset(work[i]->storage, static_cast<int>(i + 1), sizeof(work[i]->storage));
	}
	check(mem::page::free_count() == initial - work.size(), "one page per CPU, no overlap with reserved pages");
	// Mix raw pages with both a tiny and the previous-sized slab allocation.
	void* const small = mem::slab::alloc(32); void* const old = mem::slab::alloc(1544);
	check(small && old, "raw workspace pages coexist with slab buckets");
	for (unsigned i = 0; i < work.size(); ++i) {
		check(work[i]->storage[0] == i + 1 && work[i]->storage[sizeof(work[i]->storage) - 1] == i + 1,
		      "distinct workspaces retain contents");
		work[i]->~workspace(); mem::slab::free(work[i]);
	}
	check(mem::page::free_count() == initial - 2, "all raw pages reclaimed; slab bucket pages retained");
	mem::slab::free(small); mem::slab::free(old);
	std::vector<void*> remaining;
	while (void* const p = mem::page::alloc()) remaining.push_back(p);
	check(mem::page::free_count() == 0 && mem::slab::alloc(sizeof(workspace)) == nullptr, "exhaustion returns null");
	for (void* const p : remaining) mem::page::free(p);
	void* const recovered = mem::slab::alloc(sizeof(workspace));
	check(recovered != nullptr, "allocation recovers after exhaustion"); mem::slab::free(recovered);
	check(mem::page::free_count() == initial - 2, "no raw-page leak after recovery");
	std::cout << "ALLOCATOR PASS (real slab/page, workspace_bytes=" << sizeof(workspace)
	          << ", cpus=16, pages_per_cpu=1, exhaustion/recovery)\n";
}
