#pragma once

#include <random>
#include <vector>
#include <iostream>
#include <cmath>

#include "float.hpp"
#include "vk_manager.hpp"
#include "scene_loader.hpp"

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
		const Mesh& mesh);

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
		const Mesh& mesh);

	void create_command_buffer(const class ShadowProcessor& sp);

	void fill_command_buffer(const ShadowProcessor& sp);

	void compute_frame(const Vec3& suns_direction);

	MeshBuffers& get_mesh()
	{
		return scene_mesh;
	}

	VkDeviceMemory get_result()
	{
		return result_buf.mem.get();
	}

private:
	uint32_t qf_idx;
	std::vector<VkQueue> qs;

	MeshBuffers scene_mesh;

	// Global task information,
	// UniformDataInput structure,
	// whose memory will remain mapped through
	// the existence of this object.
	Buffer global_buf;
	MemMapper global_map;

	Buffer result_buf;

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

	// TODO: Temporary:
	VkDevice d;
	Buffer img_read_buf;
	std::mt19937 rnd;
	std::uniform_int_distribution<> dis{0, 50000};
	size_t pcount = 0;
};

// Optimizes the split of work groups
// for a given total work size.
struct WorkGroupSplit
{
	WorkGroupSplit(const VkPhysicalDeviceLimits &dlimits,
		uint32_t work_size);

	// Size of the local group (only x dimension used):
	uint32_t group_x_size;

	// Number of groups to dispatch:
	uint32_t num_groups;
};

// If moved, the only valid operation is destruction.
class ShadowProcessor
{
public:
	ShadowProcessor(
		VkPhysicalDevice pdevice,
		const VkPhysicalDeviceProperties &pd_props,
		UVkDevice&& device,
		std::vector<std::pair<uint32_t,
			std::vector<VkQueue>>>&& queues,
		const Mesh &mesh);

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

	size_t get_process_count() const
	{
		return count;
	}

	void accumulate_result(double *accum);

	void dump_vtk(const char* fname, double *result);

private:
	friend class QueueFamilyManager;

	// Number of points to compute:
	uint32_t num_points;

	const WorkGroupSplit wsplit;

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
