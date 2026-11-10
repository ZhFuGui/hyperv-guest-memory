#include "vcpu.hpp"
#include "../vmexit/hypercall/batch.hpp"
#include "../mem/alloc/slab.hpp"
#include <boot_info.hpp>
#include <vector.hpp>
#include <intrin.h>

namespace
{
	std::uint32_t curr_cpu_id_gs_off = 0;
	cstd::vector<vcpu> vcpus;
}

void vcpu::init(const boot_info& bi)
{
	curr_cpu_id_gs_off = bi.curr_cpu_id_gs_off;
	vcpus.resize(bi.cpu_count);
	for (auto& cpu : vcpus)
	{
		if (!cpu.batch_work)
			if (void* const storage = mem::slab::alloc(sizeof(hc::batch::workspace)))
				cpu.batch_work = new(storage) hc::batch::workspace{};
		// Allocation failure disables batching on this CPU, without breaking boot.
	}
}

vcpu::vcpu(vcpu&& other) noexcept
	: flush_needed(other.flush_needed.load()), batch_work(other.batch_work)
{
	other.batch_work = nullptr;
}

vcpu& vcpu::operator=(vcpu&& other) noexcept
{
	if (this != &other)
	{
		mem::slab::free(batch_work);
		flush_needed.store(other.flush_needed.load());
		batch_work = other.batch_work;
		other.batch_work = nullptr;
	}
	return *this;
}

vcpu::~vcpu()
{
	mem::slab::free(batch_work);
}

std::size_t vcpu::count()
{
	return vcpus.size();
}

std::size_t vcpu::curr_id()
{
	return __readgsdword(curr_cpu_id_gs_off);
}

bool vcpu::is_bsp()
{
	constexpr std::size_t bsp_id = 0;

	return curr_id() == bsp_id;
}

vcpu& vcpu::curr()
{
	return vcpus[curr_id()];
}

vcpu& vcpu::get(const std::size_t id)
{
	return vcpus[id];
}
