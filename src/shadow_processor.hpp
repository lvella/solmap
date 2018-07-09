#pragma once

#include <vector>
#include <iostream>
#include <cmath>

#include "float.hpp"
#include "vk_manager.hpp"

extern "C" {
#include "sun_position.h"
}

struct Buffer
{
	Buffer(VkDevice device,
		const VkPhysicalDeviceMemoryProperties& mem_props,
		VkBufferUsageFlags usage, uint32_t size);

	UVkDeviceMemory mem;
	UVkBuffer buf;
};

struct MeshBuffers
{
	MeshBuffers(VkDevice device,
		const VkPhysicalDeviceMemoryProperties& mem_props,
		const class aiMesh* mesh);

	Buffer vertex;
	Buffer index;
};

class QueueFamilyManager
{
public:
	QueueFamilyManager(
		VkDevice device,
		const VkPhysicalDeviceMemoryProperties& mem_props,
		uint32_t idx,
		std::vector<VkQueue>&& queues,
		const class aiScene* scene);

private:
	uint32_t qf_idx;
	std::vector<VkQueue> qs;

	std::vector<MeshBuffers> meshes;
};

// If moved, the only valid operation is destruction.
class ShadowProcessor
{
public:
	ShadowProcessor(
		VkPhysicalDevice pdevice,
		UVkDevice&& device,
		std::vector<std::pair<uint32_t,
			std::vector<VkQueue>>>&& queues,
		const class aiScene* scene);

	ShadowProcessor(ShadowProcessor&& other) = default;
	ShadowProcessor &operator=(ShadowProcessor&& other) = default;

	~ShadowProcessor()
	{
		if(d) {
			vkDeviceWaitIdle(d.get());
		}
	}

	void process(const AngularPosition& p);

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
	std::vector<QueueFamilyManager> qfs;

	Vec3 sum = {0,0,0};
	size_t count = 0;
};

