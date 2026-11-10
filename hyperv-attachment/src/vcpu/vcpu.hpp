#pragma once
#include <cstdint>
#include <cstddef>
#include <atomic.hpp>

struct boot_info;
namespace hc::batch { struct workspace; }

struct vcpu
{
	vcpu() = default;
	vcpu(vcpu&& other) noexcept;
	vcpu& operator=(vcpu&& other) noexcept;
	~vcpu();

	static void init(const boot_info& bi);

	static std::size_t count();
	static std::size_t curr_id();
	static bool is_bsp();
	static vcpu& curr();
	static vcpu& get(std::size_t id);

	template <typename F>
	static void foreach(F&& fn)
	{
		for (std::size_t i = 0; i < count(); ++i)
			fn(get(i), i);
	}

	template <typename F>
	static void foreach_but_self(F&& fn)
	{
		const auto self = curr_id();

		for (std::size_t i = 0; i < count(); ++i)
		{
			if (i == self)
				continue;

			fn(get(i), i);
		}
	}

	cstd::atomic<bool> flush_needed{};
	hc::batch::workspace* batch_work = nullptr;
};
