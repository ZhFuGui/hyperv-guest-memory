#pragma once
#include <cstdint>
#include <cstddef>
#include <atomic>

struct boot_info;

struct vcpu
{
	vcpu() = default;
	vcpu(vcpu&& other) noexcept : observed_epoch(other.observed_epoch.load()) {}
	vcpu& operator=(vcpu&& other) noexcept { observed_epoch.store(other.observed_epoch.load()); return *this; }

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

	std::atomic<std::uint64_t> observed_epoch{};
	std::uint64_t retry_gpa = UINT64_MAX;
	std::uint64_t retry_rip = 0;
	std::uint64_t retry_root = 0;
};
