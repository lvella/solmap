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
		VkBufferUsageFlags usage, uint32_t size,
		VkMemoryPropertyFlags required,
		VkMemoryPropertyFlags prefered);

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
	uint32_t idx_count;
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

	void create_command_buffer(const class ShadowProcessor& sp);

	void fill_command_buffer(VkRenderPass rp,
		VkPipeline pipeline, VkPipelineLayout pipeline_layout);

	void render_frame(const Vec3& suns_direction);

private:
	uint32_t qf_idx;
	std::vector<VkQueue> qs;

	std::vector<MeshBuffers> meshes;

	// Memory will remain mapped through
	// the existence of this object.
	Buffer uniform_buf;
	MemMapper uniform_map;

	UVkDescriptorPool desc_pool;
	VkDescriptorSet uniform_desc_set;

	UVkImage depth_image;
	UVkDeviceMemory depth_image_mem;
	UVkImageView depth_image_view;
	UVkFramebuffer framebuffer;
	VkDescriptorSet img_sampler_desc_set;

	UVkCommandPool command_pool;
	UVkCommandBuffers cmd_bufs;
	//UVkFence frame_fence;
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
	friend class QueueFamilyManager;

	void create_render_pipeline();
	void create_compute_pipeline();

	UVkDevice d;

	// Stuf common to both pipelines:
	UVkDescriptorSetLayout uniform_desc_set_layout;

	// Graphics pipeline stuff:
	UVkShaderModule vert_shader;
	UVkRenderPass render_pass;
	UVkPipelineLayout graphic_pipeline_layout;
	UVkGraphicsPipeline graphic_pipeline;

	// Compute pipeline stuff:
	UVkShaderModule compute_shader;
	UVkSampler depth_sampler;
	UVkDescriptorSetLayout comp_sampler_dset_layout;
	UVkPipelineLayout compute_pipeline_layout;
	UVkComputePipeline compute_pipeline;

	std::vector<QueueFamilyManager> qfs;

	Vec3 sum = {0,0,0};
	size_t count = 0;
};

