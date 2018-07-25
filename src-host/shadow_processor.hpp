#pragma once

#include <vector>
#include <queue>
#include <iostream>
#include <cmath>

#include "float.hpp"
#include "vk_manager.hpp"
#include "scene_loader.hpp"
#include "buffer.hpp"

extern "C" {
#include "sun_position.h"
}

struct MeshBuffers
{
	MeshBuffers(VkDevice device,
		const VkPhysicalDeviceMemoryProperties& mem_props,
		const Mesh& mesh);

	Buffer vertex;
	Buffer index;
	uint32_t idx_count;
};

class TaskSlot
{
public:
	TaskSlot(VkDevice device,
		const VkPhysicalDeviceMemoryProperties& mem_props,
		uint32_t idx,
		VkQueue graphic_queue,
		const Mesh& mesh);

	void create_command_buffer(
		const VkPhysicalDeviceMemoryProperties& mem_props,
		const class ShadowProcessor& sp, VkCommandPool command_pool,
		BufferTransferer &btransf);

	void fill_command_buffer(const ShadowProcessor& sp);

	void compute_frame(const Vec3& suns_direction);

	VkFence get_fence()
	{
		return frame_fence.get();
	}

	MeshBuffers& get_mesh()
	{
		return scene_mesh;
	}

	void accumulate_result(VkDevice d,
		const VkPhysicalDeviceMemoryProperties& mem_props,
		BufferTransferer& btransf,
		uint32_t count, double* accum);

private:
	uint32_t qf_idx;
	VkQueue queue;

	MeshBuffers scene_mesh;

	// Global task information,
	// UniformDataInput structure,
	// whose memory will remain mapped through
	// the existence of this object.
	MaybeStagedBuffer global_buf;
	MemMapper global_map;

	AccessibleBuffer result_buf;

	VkDescriptorSet global_desc_set;

	UVkImage depth_image;
	UVkDeviceMemory depth_image_mem;
	UVkImageView depth_image_view;
	UVkFramebuffer framebuffer;
	VkDescriptorSet compute_desc_set;

	UVkCommandBuffers cmd_bufs;
	UVkFence frame_fence;
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

	const std::string& get_name()
	{
		return device_name;
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
	friend class TaskSlot;

	// Empirically, 5 seems to be the optimal number
	// in a GeForce GTX 760. Maybe this number should
	// be emprirically optimized between runs.
	static const unsigned SLOTS_PER_QUEUE = 5;

	std::string device_name;

	// Number of points to compute:
	uint32_t num_points;

	const WorkGroupSplit wsplit;

	void create_render_pipeline();
	void create_compute_pipeline();

	UVkDevice d;
	VkPhysicalDeviceMemoryProperties mem_props;

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

	// Memory pools:
	UVkDescriptorPool desc_pool;
	// One per queue family
	// (although I never saw more than one graphics QF per device,
	// maybe with SLI/Crossfire?)
	std::vector<UVkCommandPool> command_pool;

	std::vector<TaskSlot> task_pool;
	std::queue<uint32_t> available_slots;
	std::vector<VkFence> fence_set;

	Vec3 sum = {0,0,0};
	size_t count = 0;
};

