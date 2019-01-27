#pragma once

#include <vector>
#include <queue>
#include <iostream>
#include <cmath>

#include "float.hpp"
#include "vk_manager.hpp"
#include "mesh_tools.hpp"
#include "buffer.hpp"

extern "C" {
#include "sun_position.h"
}

struct MeshBuffers
{
	MeshBuffers(VkDevice device,
		const VkPhysicalDeviceMemoryProperties& mem_props,
		const Mesh& mesh,  BufferTransferer& btransf);

	AccessibleBuffer vertex;
	AccessibleBuffer index;
	uint32_t idx_count;
};

class TaskSlot
{
public:
	TaskSlot(VkDevice device,
		const VkPhysicalDeviceMemoryProperties& mem_props,
		uint32_t idx, uint32_t num_points,
		VkQueue graphic_queue);

	void create_command_buffer(
		const class ShadowProcessor& sp,
		VkCommandPool command_pool,
		VkBuffer test_set,
		BufferTransferer &btransf);

	void fill_command_buffer(const ShadowProcessor& sp,
		const MeshBuffers &mesh);

	void compute_frame(const Vec3& sun_direction, const Vec3& denergy);

	VkFence get_fence()
	{
		return frame_fence.get();
	}

	VkQueue get_queue()
	{
		return queue;
	}

	void accumulate_result(BufferTransferer& btransf,
		uint32_t count, Vec3* accum);

private:
	uint32_t qf_idx;
	VkQueue queue;

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
		const Mesh &mesh,
		const std::vector<VertexData>& test_set);

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

	void process(const Vec3& suns_direction, const InstantaneousData& instant);

	const Vec3& get_directional_sum() const
	{
		return directional_sum;
	}

	double get_diffuse_sum() const
	{
		return diffuse_sum;
	}

	double get_time_sum() const
	{
		return time_sum;
	}

	size_t get_process_count() const
	{
		return count;
	}

	void accumulate_result(Vec3 *accum);

private:
	friend class TaskSlot;

	// Empirically, 5 seems to be the optimal number
	// in a GeForce GTX 760. Maybe this number should
	// be dynamically optimized between runs.
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

	// Const data, one per queue family:
	std::vector<MeshBuffers> mesh;
	std::vector<AccessibleBuffer> test_buffer;

	// Memory pools:
	UVkDescriptorPool desc_pool;
	// One per queue family
	// (although I never saw more than one graphics QF per device,
	// maybe with SLI/Crossfire?)
	std::vector<UVkCommandPool> command_pool;

	std::vector<TaskSlot> task_pool;
	std::queue<uint32_t> available_slots;
	std::vector<VkFence> fence_set;

	Vec3 directional_sum = {0,0,0};
	double diffuse_sum = 0.0;
	double time_sum = 0.0;
	size_t count = 0;
};

