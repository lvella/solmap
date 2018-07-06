#pragma once

#include <vector>
#include <iostream>
#include "vec3.hpp"
#include "vk_manager.hpp"

extern "C" {
#include "sun_position.h"
}

// If moved, the only valid operation is destruction.
class ShadowProcessor
{
public:
	ShadowProcessor(UVkDevice&& device, std::vector<VkQueue>&& queues):
		d{std::move(device)},
		qs{std::move(queues)}
	{
		std::cout << "### Total compute queues: " << qs.size() << '\n';
	}

	ShadowProcessor(ShadowProcessor&& other) = default;
	ShadowProcessor &operator=(ShadowProcessor&& other) = default;

	~ShadowProcessor()
	{
		if(d) {
			vkDeviceWaitIdle(d.get());
		}
	}

	void process(const AngularPosition& p)
	{
		// Transforms into a unit vector pointing to the sun.
		// We convention Y as upwards and -z as N, thus
		// lookig from above, we have:
		//
		//        -z, N
		//           |
		//           |
		// -x, O ----+---- +x, E
		//           |
		//           |
		//        +z, S
                double c = cos(p.alt);

		Vec3 sun{
			sin(p.az) * c,
			sin(p.alt),
			-cos(p.az) * c
		};

		sum += sun;
		++count;
	}

	const Vec3& get_sum() const
	{
		return sum;
	}

	size_t get_count() const
	{
		return count;
	}

private:
	UVkDevice d;
	std::vector<VkQueue> qs;

	Vec3 sum;
	size_t count = 0;
};

