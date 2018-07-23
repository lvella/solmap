#pragma once

#include <vector>
#include <queue>
#include <iostream>
#include <cmath>

#include "float.hpp"
#include "vk_manager.hpp"
#include "scene_loader.hpp"

extern "C" {
#include "sun_position.h"
}

enum BufferAccessDirection {
	HOST_CAN_WRITE_BIT = 0x1,
	HOST_CAN_READ_BIT = 0x2
};

struct Buffer
{
	Buffer() = default;
	Buffer(VkDevice device,
		const VkPhysicalDeviceMemoryProperties& mem_props,
		VkBufferUsageFlags usage, uint32_t size,
		VkMemoryPropertyFlags required,
		VkMemoryPropertyFlags prefered);

	UVkDeviceMemory mem;
	UVkBuffer buf;
};

// Device local buffer which can be accessed from host,
// either directly, with host visibility, or via transfer.
// Automatically sets buffer usage flags for transfer,
// if needed.
struct AccessibleBuffer: public Buffer
{
	AccessibleBuffer(VkDevice d,
		const VkPhysicalDeviceMemoryProperties& mem_props,
		VkBufferUsageFlags usage, uint32_t size,
		BufferAccessDirection host_direction);

	bool is_host_visible;
};

// Device local buffer, with staging buffer if there is
// no available host visible memory type.
struct MaybeStagedBuffer: public Buffer
{
	MaybeStagedBuffer(VkDevice device,
		const VkPhysicalDeviceMemoryProperties& mem_props,
		VkBufferUsageFlags usage, uint32_t size,
		BufferAccessDirection host_direction);
	std::unique_ptr<Buffer> staging_buf;
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

class TaskSlot
{
public:
	TaskSlot(
		VkDevice device,
		const VkPhysicalDeviceMemoryProperties& mem_props,
		uint32_t idx,
		VkQueue graphic_queue,
		const Mesh& mesh);

	void create_command_buffer(
		const class ShadowProcessor& sp, VkCommandPool command_pool);

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

	VkDeviceMemory get_result()
	{
		return result_buf.mem.get();
	}

private:
	uint32_t qf_idx;
	VkQueue queue;

	MeshBuffers scene_mesh;

	// Global task information,
	// UniformDataInput structure,
	// whose memory will remain mapped through
	// the existence of this object.
	Buffer global_buf;
	MemMapper global_map;
	// Staging buffer for the global buffer, only used
	// if the device doesn't have a (device local + host visible)
	// memory type:
	std::unique_ptr<Buffer> staging_buf;

	Buffer result_buf;

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
	static const unsigned SLOTS_PER_QUEUE = 3;

	std::string device_name;

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

