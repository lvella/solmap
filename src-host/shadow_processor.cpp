#include <fstream>
#include <cstddef>

#include <assimp/scene.h>
#include <glm/glm.hpp>

#include "shadow_processor.hpp"

static const uint32_t frame_size = 2048;

struct GlobalInputData
{
	Vec4 orientation;
	Vec3 suns_direction;
};

template <typename T1, typename T2>
uint32_t ptr_delta(const T1* from, const T2* to)
{
	return static_cast<uint32_t>(
		reinterpret_cast<const uint8_t*>(to)
		- reinterpret_cast<const uint8_t*>(from)
	);
}

// Get quaternion rotation from unit vector a to unit vector b.
// Doesn't work if a and b are opposites.
// Lets assume, for now, that it never happens.
static Vec4 rot_from_unit_a_to_unit_b(Vec3 a, Vec3 b)
{
	Vec4 ret{glm::cross(a,b), 1.0 + glm::dot(a, b)};
	return glm::normalize(ret);
}

MeshBuffers::MeshBuffers(VkDevice device,
	const VkPhysicalDeviceMemoryProperties& mem_props,
	const Mesh& mesh
):
	vertex(device, mem_props,
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
		// TODO: temporary, while there isn't a separated
		// buffer for points to be tested, use the same
		// buffer for vertex input and to compute shader:
		| VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		mesh.vertices.size() * sizeof(VertexData),
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
	),
	index(device, mem_props,
		VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
		mesh.indices.size() * sizeof(uint32_t),
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
	),
	idx_count(mesh.indices.size())
{
	{
		// Copy the vertex data to device memory.
		MemMapper map(device, vertex.mem.get());
		std::copy(mesh.vertices.begin(), mesh.vertices.end(),
			map.get<VertexData*>());
	}

	{
		// Copy the index data to device memory.
		MemMapper map(device, index.mem.get());
		std::copy(mesh.indices.begin(), mesh.indices.end(),
			map.get<uint32_t*>());
	}
}

TaskSlot::TaskSlot(
	VkDevice device,
	const VkPhysicalDeviceMemoryProperties& mem_props,
	uint32_t idx,
	VkQueue graphic_queue,
	const Mesh &mesh
):
	qf_idx{idx},
	queue{graphic_queue},
	scene_mesh{device, mem_props, mesh},
	global_buf{device, mem_props,
		VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		sizeof(GlobalInputData),
		HOST_WILL_WRITE_BIT
	},
	global_map{device, global_buf.get_visible_mem()},
	result_buf{device, mem_props,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		static_cast<uint32_t>(mesh.vertices.size() * sizeof(float)),
		BufferAccessDirection(HOST_WILL_WRITE_BIT | HOST_WILL_READ_BIT)
	}
{
	// Create the depth image, used rendering destination and output.
	depth_image = UVkImage{VkImageCreateInfo{
		VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		nullptr,
		0,
		VK_IMAGE_TYPE_2D, // imageType
		VK_FORMAT_D32_SFLOAT, // format
		{
			frame_size, // width
			frame_size, // height
			1 // depth
		}, // extent
		1, // mipLevels
		1, // arrayLayers
		VK_SAMPLE_COUNT_1_BIT, // samples
		VK_IMAGE_TILING_OPTIMAL, // tiling
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
		| VK_IMAGE_USAGE_SAMPLED_BIT, // usage
		VK_SHARING_MODE_EXCLUSIVE, // sharing
		0, // queueFamilyIndexCount
		nullptr, // pQueueFamilyIndices
		VK_IMAGE_LAYOUT_UNDEFINED // initialLayout
	}, device};

	// Allocate depth image memory.
	VkMemoryRequirements reqs;
	vkGetImageMemoryRequirements(device, depth_image.get(), &reqs);

	// Find a suitable heap. No specific needs, but prefer it to be local.
	uint32_t mtype = find_memory_heap(
		mem_props,
		reqs.memoryTypeBits,
		0,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
	);

	// Allocate the image memory
	depth_image_mem = UVkDeviceMemory(VkMemoryAllocateInfo{
		VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		nullptr,
		reqs.size,
		mtype
	}, device);

	// Bind the memory to the image
	vkBindImageMemory(device, depth_image.get(), depth_image_mem.get(), 0);

	// Create the image view:
	depth_image_view = UVkImageView{VkImageViewCreateInfo{
		VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		nullptr,
		0,
		depth_image.get(),
		VK_IMAGE_VIEW_TYPE_2D,
		VK_FORMAT_D32_SFLOAT,
		{
			VK_COMPONENT_SWIZZLE_IDENTITY,
			VK_COMPONENT_SWIZZLE_IDENTITY,
			VK_COMPONENT_SWIZZLE_IDENTITY,
			VK_COMPONENT_SWIZZLE_IDENTITY
		},
		{
			VK_IMAGE_ASPECT_DEPTH_BIT,
			0, 1, 0, 1
		}
	}, device};

	// Create the frame fence
	frame_fence = UVkFence(VkFenceCreateInfo{
		VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		nullptr,
		VK_FENCE_CREATE_SIGNALED_BIT
	}, device);
}

void TaskSlot::create_command_buffer(
	const VkPhysicalDeviceMemoryProperties& mem_props,
	const ShadowProcessor& sp, VkCommandPool command_pool,
	BufferTransferer &btransf)
{
	// Create the framebuffer:
	auto at = depth_image_view.get();
	framebuffer = UVkFramebuffer{VkFramebufferCreateInfo{
		VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		nullptr,
		0,
		sp.render_pass.get(),
		1,
		&at,
		frame_size,
		frame_size,
		1
	}, sp.d.get()};

	// Create descriptor set, for uniform variable
	const VkDescriptorSetLayout dset_layouts[] = {
		sp.uniform_desc_set_layout.get(),
		sp.comp_sampler_dset_layout.get()
	};
	const VkDescriptorSetAllocateInfo dsai {
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		nullptr,
		sp.desc_pool.get(),
		(sizeof dset_layouts) / (sizeof dset_layouts[0]),
		dset_layouts
	};

	VkDescriptorSet dsets[2];
	chk_vk(vkAllocateDescriptorSets(sp.d.get(), &dsai, dsets));
	global_desc_set = dsets[0];
	compute_desc_set = dsets[1];

	const VkDescriptorBufferInfo buffer_info {
		global_buf.buf.get(),
		0,
		VK_WHOLE_SIZE
	};

	const VkDescriptorImageInfo img_info {
		// sampler, unused because it is immutable, but set anyway:
		sp.depth_sampler.get(),
		depth_image_view.get(),
		VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL
	};

	const VkDescriptorBufferInfo input_points_binfo {
		scene_mesh.vertex.buf.get(),
		0,
		VK_WHOLE_SIZE
	};

	const VkDescriptorBufferInfo result_binfo {
		result_buf.buf.get(),
		0,
		VK_WHOLE_SIZE
	};

	const VkWriteDescriptorSet wds[] = {
	       	{
			VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			nullptr,
			global_desc_set,
			0,
			0,
			1,
			VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			nullptr,
			&buffer_info,
			nullptr
		},
		{
			VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			nullptr,
			compute_desc_set,
			0,
			0,
			1,
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			&img_info,
			nullptr,
			nullptr
		},
		{
			VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			nullptr,
			compute_desc_set,
			1,
			0,
			1,
			VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			nullptr,
			&input_points_binfo,
			nullptr
		},
		{
			VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			nullptr,
			compute_desc_set,
			2,
			0,
			1,
			VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			nullptr,
			&result_binfo,
			nullptr
		}
	};

	vkUpdateDescriptorSets(sp.d.get(),
		(sizeof wds) / (sizeof wds[0]), wds, 0, nullptr);

	// Allocate a single command buffer:
	cmd_bufs = UVkCommandBuffers(sp.d.get(), VkCommandBufferAllocateInfo{
		VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		nullptr,
		command_pool,
		VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		1
	});

	// Zero the result buffer
	if(result_buf.is_host_visible){
		MemMapper map{sp.d.get(), result_buf.mem.get()};
		std::fill_n(map.get<float*>(),
			sp.num_points,
			0.0f
		);
	} else {
		btransf.transfer<float*>(sp.d.get(), mem_props, cmd_bufs[0],
			queue, result_buf.buf.get(),
			sizeof(float) * sp.num_points,
			HOST_WILL_WRITE_BIT,
			[&](float* ptr) {
				std::fill_n(ptr,
					sp.num_points,
					0.0f
				);
			}
		);
	}

	fill_command_buffer(sp);
}

void TaskSlot::fill_command_buffer(
		const ShadowProcessor& sp)
{
	// Start recording the commands in the command buffer.
	VkCommandBufferBeginInfo cbbi{
		VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		nullptr,
		0,
		nullptr
	};
	chk_vk(vkBeginCommandBuffer(cmd_bufs[0], &cbbi));

	// If using staging buffer, issue the transfer:
	if(global_buf.staging_buf) {
		const VkBufferCopy region {
			0, 0, sizeof(GlobalInputData)
		};
		vkCmdCopyBuffer(cmd_bufs[0],
			global_buf.staging_buf->buf.get(),
			global_buf.buf.get(),
			1, &region
		);
	}

	// Draw the depth buffer command:
	VkClearValue cv;
	cv.depthStencil = {1.0, 0};

	VkRenderPassBeginInfo rpbi {
		VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		nullptr,
		sp.render_pass.get(),
		framebuffer.get(),
		{
			{0, 0},
			{frame_size, frame_size}
		},
		1,
		&cv
	};
	vkCmdBeginRenderPass(cmd_bufs[0], &rpbi, VK_SUBPASS_CONTENTS_INLINE);

	// Bind the pipeline:
	vkCmdBindPipeline(cmd_bufs[0], VK_PIPELINE_BIND_POINT_GRAPHICS, sp.graphic_pipeline.get());

	// Bind the uniform variable.
	vkCmdBindDescriptorSets(cmd_bufs[0],
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		sp.graphic_pipeline_layout.get(), 0, 1, &global_desc_set, 0, nullptr);

	// Draw things:
	const VkDeviceSize zero_offset = 0;

	// Bind vertex buffer.
	vkCmdBindVertexBuffers(cmd_bufs[0], 0, 1,
		&scene_mesh.vertex.buf.get(), &zero_offset);

	// Bind index buffer.
	vkCmdBindIndexBuffer(cmd_bufs[0],
		scene_mesh.index.buf.get(), 0, VK_INDEX_TYPE_UINT32);

	// Draw the object:
	vkCmdDrawIndexed(cmd_bufs[0], scene_mesh.idx_count, 1, 0, 0, 0);

	// End drawing stuff.
	vkCmdEndRenderPass(cmd_bufs[0]);

	// Begin incidence compute:

	// Bind compute pipeline:
	vkCmdBindPipeline(cmd_bufs[0], VK_PIPELINE_BIND_POINT_COMPUTE,
		sp.compute_pipeline.get());

	// Bind both descriptor sets to the compute pipeline
	VkDescriptorSet dsets[] = {
		global_desc_set,
		compute_desc_set
	};
	vkCmdBindDescriptorSets(cmd_bufs[0],
		VK_PIPELINE_BIND_POINT_COMPUTE,
		sp.compute_pipeline_layout.get(),
		0,
		(sizeof dsets) / (sizeof dsets[0]), dsets,
		0, nullptr
	);

	// Perform the compute:
	vkCmdDispatch(cmd_bufs[0], sp.wsplit.num_groups, 1, 1);

	// End compute phase.

	// End command buffer.
	chk_vk(vkEndCommandBuffer(cmd_bufs[0]));
}

void TaskSlot::compute_frame(const Vec3& direction)
{
	// Get pointer to device memory:
	auto params = global_map.get<GlobalInputData*>();

	// The rotation from model space sun to (0, 0, -1),
	// which is pointing to the viewer in Vulkan coordinates.
	params->orientation = rot_from_unit_a_to_unit_b(
		direction, Vec3{0.0, 0.0, -1.0});

	// Set sun's direction:
	params->suns_direction = direction;

	// Flush the copy.
	global_map.flush();

	const VkSubmitInfo si{
		VK_STRUCTURE_TYPE_SUBMIT_INFO,
		nullptr,
		0,
		nullptr,
		nullptr,
		1,
		&cmd_bufs[0],
		0,
		nullptr
	};

	vkQueueSubmit(queue, 1, &si, frame_fence.get());
}

void TaskSlot::accumulate_result(VkDevice d,
	const VkPhysicalDeviceMemoryProperties& mem_props,
	BufferTransferer& btransf,
	uint32_t count, double* accum)
{
	if(result_buf.is_host_visible) {
		MemMapper map{d, result_buf.mem.get()};
		auto ptr = map.get<float*>();
		for(uint32_t i = 0; i < count; ++i) {
			accum[i] += ptr[i];
		}
	} else {
		btransf.transfer<float*>(d, mem_props, cmd_bufs[0],
			queue, result_buf.buf.get(), count,
			HOST_WILL_READ_BIT, [&](float* ptr) {
				for(uint32_t i = 0; i < count; ++i) {
					accum[i] += ptr[i];
				}
			}
		);
	}
}

WorkGroupSplit::WorkGroupSplit(const VkPhysicalDeviceLimits &dlimits,
	uint32_t work_size)
{
	const uint32_t limit = std::max(
		dlimits.maxComputeWorkGroupInvocations,
		dlimits.maxComputeWorkGroupSize[0]
	);

	num_groups = work_size / limit + (work_size % limit > 0);
	group_x_size = work_size / num_groups + (work_size % num_groups > 0);
}

ShadowProcessor::ShadowProcessor(
	VkPhysicalDevice pdevice,
	const VkPhysicalDeviceProperties &pd_props,
	UVkDevice&& device,
	std::vector<std::pair<uint32_t, std::vector<VkQueue>>>&& queues,
	const Mesh &mesh
):
	device_name{pd_props.deviceName},
	num_points{static_cast<uint32_t>(mesh.vertices.size())},
	wsplit{pd_props.limits, num_points},
	d{std::move(device)}
{
	// Create depth buffer rendering pipeline:
	create_render_pipeline();

	// Create solar incidence compute pipeline:
	create_compute_pipeline();

	unsigned num_slots = 0;
	for(auto &qf: queues) {
		num_slots = qf.second.size() * SLOTS_PER_QUEUE;
	}

	// Create the allocation pools.
	// Allocation pool for descriptors:
	const VkDescriptorPoolSize dps[] = {
	       	{
			VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			num_slots
		},
		{
			VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			2 * num_slots
		},
	       	{
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			num_slots
		}
	};
	desc_pool = UVkDescriptorPool(VkDescriptorPoolCreateInfo{
		VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		nullptr,
		0,
		2,
		(sizeof dps) / (sizeof dps[0]),
		dps
	}, d.get());

	// Get the memory properties needed to allocate the buffer.
	vkGetPhysicalDeviceMemoryProperties(pdevice, &mem_props);

	command_pool.reserve(queues.size());
	task_pool.reserve(num_slots);
	for(auto &qf: queues) {
		// Allocation pool for command buffer:
		command_pool.push_back(UVkCommandPool{VkCommandPoolCreateInfo{
			VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			nullptr,
			0,
			qf.first
		}, d.get()});

		// Create one task slot per queue,
		// the buffers will be local to it.
		BufferTransferer btransf;
		for(auto& q: qf.second)
		{
			task_pool.emplace_back(d.get(),
				mem_props, qf.first, q, mesh);

			task_pool.back().create_command_buffer(mem_props,
				*this, command_pool.back().get(),
				btransf
			);
			fence_set.push_back(task_pool.back().get_fence());
			available_slots.push(task_pool.size()-1);
		}
	}
}

void ShadowProcessor::create_render_pipeline()
{
	// Create the vertex shader:
	static const uint32_t vert_shader_data[] =
		#include "depth-map.vert.inc"
	;

	vert_shader = UVkShaderModule(VkShaderModuleCreateInfo {
		VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		nullptr,
		0,
		sizeof vert_shader_data,
		vert_shader_data
	}, d.get());

	const VkPipelineShaderStageCreateInfo pss {
		VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		nullptr,
		0,
		VK_SHADER_STAGE_VERTEX_BIT,
		vert_shader.get(),
		"main",
		nullptr
	};

	// Vertex data description:
	const VkVertexInputBindingDescription vibd {
		0,
		sizeof(VertexData),
		VK_VERTEX_INPUT_RATE_VERTEX
	};

	// Position attribute in vertex data:
	const VkVertexInputAttributeDescription viad {
		0,
		0,
		VK_FORMAT_R32G32B32_SFLOAT,
		0,
	};

	// Vertex input description:
	const VkPipelineVertexInputStateCreateInfo pvis {
		VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		nullptr,
		0,
		1,
		&vibd,
		1,
		&viad
	};

	// Primitive assembly description
	const VkPipelineInputAssemblyStateCreateInfo pias {
		VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		nullptr,
		0,
		VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		VK_FALSE
	};

	// Viweport creation:
	const VkViewport viewport {
		0.0,
		0.0,
		frame_size,
		frame_size,
		0.0,
		1.0
	};

	const VkRect2D scissor = {
		{0, 0},
		{frame_size, frame_size}
	};

	const VkPipelineViewportStateCreateInfo pvs {
		VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		nullptr,
		0,
		1,
		&viewport,
		1,
		&scissor
	};

	// Rasterization configuration:
	const VkPipelineRasterizationStateCreateInfo prs {
		VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		nullptr,
		0,
		VK_FALSE,
		VK_FALSE,
		VK_POLYGON_MODE_FILL,
		VK_CULL_MODE_BACK_BIT,
		VK_FRONT_FACE_COUNTER_CLOCKWISE,
		VK_FALSE,
		0.0,
		0.0,
		0.0,
		1.0
	};

	// Multisampling configuration:
	const VkPipelineMultisampleStateCreateInfo pms {
		VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		nullptr,
		0,
		VK_SAMPLE_COUNT_1_BIT,
		VK_FALSE,
		1.0,
		nullptr,
		VK_FALSE,
		VK_FALSE
	};

	// Depth buffer configuration:
	const VkPipelineDepthStencilStateCreateInfo pdss {
		VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		nullptr,
		0,
		VK_TRUE,
		VK_TRUE,
		VK_COMPARE_OP_LESS,
		VK_FALSE,
		VK_FALSE,
		{},
		{},
		0.0,
		1.0
	};

	// Push constant used to push the orientation
	// quaternion into the vertex shader.
	/*const VkPushConstantRange pcr {
		VK_SHADER_STAGE_VERTEX_BIT,
		0,
		sizeof(Vec4)
	};*/

	// Uniform variable setting.
	const VkDescriptorSetLayoutBinding dslb {
		0,
		VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		1,
		VK_SHADER_STAGE_VERTEX_BIT |
		VK_SHADER_STAGE_COMPUTE_BIT,
		nullptr
	};

	uniform_desc_set_layout = UVkDescriptorSetLayout(
		VkDescriptorSetLayoutCreateInfo{
			VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			nullptr,
			0,
			1,
			&dslb
		}, d.get()
	);

	graphic_pipeline_layout = UVkPipelineLayout(VkPipelineLayoutCreateInfo{
		VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		nullptr,
		0,
		1,
		&uniform_desc_set_layout.get(),
		0, //1,
		nullptr //&pcr
	}, d.get());

	// Depth buffer attachment.
	// After the render pass, the image should be in
	// a layout to be sampled by the compute shader.
	const VkAttachmentDescription dbad {
		0,
		VK_FORMAT_D32_SFLOAT,
		VK_SAMPLE_COUNT_1_BIT,
		VK_ATTACHMENT_LOAD_OP_CLEAR,
		VK_ATTACHMENT_STORE_OP_STORE,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		VK_ATTACHMENT_STORE_OP_DONT_CARE,
		VK_IMAGE_LAYOUT_UNDEFINED,
		VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL
	};

	// Depth buffer attachment reference:
	const VkAttachmentReference dbar {
		0,
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
	};

	// Only subpass in our render pass:
	const VkSubpassDescription sd {
		0,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		0,
		nullptr,
		0,
		nullptr,
		nullptr,
		&dbar,
		0,
		nullptr
	};


	const VkSubpassDependency sdeps[] {
		// Set the vertex shader stage of the subpass as dependant
		// on any buffer transfer operation happening before it, so
		// it properly synchronizes the transfer of the uniform buffer,
		// in case a staging buffer is used.
		{
			VK_SUBPASS_EXTERNAL, // srcSubpass
			0, // dstSubpass
			VK_PIPELINE_STAGE_TRANSFER_BIT, // srcStageMask
			VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, // dstStageMask
			VK_ACCESS_TRANSFER_WRITE_BIT, // srcAccessMask
			VK_ACCESS_UNIFORM_READ_BIT, // dstAccessMask
			0 // dependencyFlags
		},
		// Set the compute shader read of the depth buffer
		// to be dependant on the graphics pipeline having finished
		// writing it.
		{
			0, // srcSubpass
			VK_SUBPASS_EXTERNAL, // dstSubpass
			VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, // srcStageMask
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, // dstStageMask
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, // srcAccessMask
			VK_ACCESS_SHADER_READ_BIT, // dstAccessMask
			0 // dependencyFlags
		}
	};

	render_pass = UVkRenderPass(VkRenderPassCreateInfo {
		VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		nullptr,
		0,
		1,
		&dbad,
		1,
		&sd,
		2,
		sdeps
	}, d.get());

	graphic_pipeline = UVkGraphicsPipeline(VkGraphicsPipelineCreateInfo{
		VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		nullptr,
		0,
		1,       // stageCount
		&pss, // pStages
		&pvis,   // pVertexInputState
		&pias,   // pInputAssemblyState
		nullptr, // pTessellationState
		&pvs,    // pViewportState
		&prs,    // pRasterizationState
		&pms,    // pMultisampleState
		&pdss,   // pDepthStencilState
		nullptr, // pColorBlendState
		nullptr, // pDynamicState
		graphic_pipeline_layout.get(), // layout
		render_pass.get(),     // renderPass
		0,                     // subpass
		VK_NULL_HANDLE, // basePipelineHandle
		-1              // basePipelineIndex
	}, d.get(), nullptr, 1);
}

void ShadowProcessor::create_compute_pipeline()
{
	// Create the compute shader:
	static const uint32_t compute_shader_data[] =
		#include "incidence-calc.comp.inc"
	;

	compute_shader = UVkShaderModule(VkShaderModuleCreateInfo {
		VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		nullptr,
		0,
		sizeof compute_shader_data,
		compute_shader_data
	}, d.get());

	depth_sampler = UVkSampler{VkSamplerCreateInfo{
		VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		nullptr,
		0,
		VK_FILTER_LINEAR,
		VK_FILTER_LINEAR,
		VK_SAMPLER_MIPMAP_MODE_NEAREST,
		VK_SAMPLER_ADDRESS_MODE_REPEAT,
		VK_SAMPLER_ADDRESS_MODE_REPEAT,
		VK_SAMPLER_ADDRESS_MODE_REPEAT,
		0.0f,
		VK_FALSE,
		1.0,
		VK_FALSE, // compareEnable, but also depends on shader...
		VK_COMPARE_OP_LESS_OR_EQUAL,
		0.0f,
		0.0f,
		VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
		VK_FALSE
	}, d.get()};

	// Build the descriptor sets for the compute shader:
	const VkDescriptorSetLayoutBinding dslbs[] = {
		// Depth image sampler:
		{
			0,
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			1,
			VK_SHADER_STAGE_COMPUTE_BIT,
			&depth_sampler.get()
		},

		// Input points (same buffer as graphics attributes):
		{
			1,
			VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			1,
			VK_SHADER_STAGE_COMPUTE_BIT,
			nullptr
		},

		// Accumulated incidence for each input point:
		{
			2,
			VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			1,
			VK_SHADER_STAGE_COMPUTE_BIT,
			nullptr
		}
	};

	comp_sampler_dset_layout = UVkDescriptorSetLayout{
		VkDescriptorSetLayoutCreateInfo{
			VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			nullptr,
			0,
			(sizeof dslbs) / (sizeof dslbs[0]),
			dslbs
		}, d.get()
	};

	// Use the same descriptor set layout for the uniform variable
	// used in the graphics pipeline, together with the new
	// descriptor set layout created for the depth texture sampler.
	VkDescriptorSetLayout dsls[] = {
		uniform_desc_set_layout.get(),
		comp_sampler_dset_layout.get()
	};

	compute_pipeline_layout = UVkPipelineLayout(VkPipelineLayoutCreateInfo{
		VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		nullptr,
		0,
		(sizeof dsls) / (sizeof dsls[0]),
		dsls,
		0,
		nullptr
	}, d.get());

	// Set the total number of points worked by this compute pipeline.
	// It is an specialization constant in the shader.
	const VkSpecializationMapEntry specializations[] = {
		{
			0,
			ptr_delta(this, &num_points),
			sizeof num_points
		},
		{
			1,
			ptr_delta(this, &wsplit.group_x_size),
			sizeof wsplit.group_x_size
		}
	};

	const VkSpecializationInfo sinfo {
		(sizeof specializations) / (sizeof specializations[0]),
		specializations,
		sizeof *this,
		this
	};

	// Finaly, create the pipeline shader.
	compute_pipeline = UVkComputePipeline{VkComputePipelineCreateInfo{
		VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		nullptr,
		0,
		{
			VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			nullptr,
			0,
			VK_SHADER_STAGE_COMPUTE_BIT,
			compute_shader.get(),
			"main",
			&sinfo
		},
		compute_pipeline_layout.get(),
		VK_NULL_HANDLE,
		-1
	}, d.get(), nullptr, 1};
}

void ShadowProcessor::process(const AngularPosition& p)
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
	real c = std::cos(p.alt);

	Vec3 sun{
		std::sin(p.az) * c,
		std::sin(p.alt),
		-std::cos(p.az) * c
	};

	sum += sun;
	++count;

	if(available_slots.empty()) {
		// No task slot available, wait on fences.
		VkResult ret;
		do {
			ret = vkWaitForFences(d.get(), fence_set.size(),
				fence_set.data(), VK_FALSE,
				1000ul*1000ul*1000ul*60ul /* one minute */);
		} while (ret == VK_TIMEOUT);
		chk_vk(ret);

		// Find what fences were signaled and make the slots available.
		for(uint32_t i = 0; i < fence_set.size(); ++i) {
			if(vkGetFenceStatus(d.get(), fence_set[i])
				== VK_SUCCESS)
			{
				available_slots.push(i);
			}
		}
	}

	// Get the next available task slot.
	uint32_t task_idx = available_slots.front();
	available_slots.pop();

	// Send the processing to that slot.
	vkResetFences(d.get(), 1, &fence_set[task_idx]);
	task_pool[task_idx].compute_frame(sun);
}

void ShadowProcessor::accumulate_result(double *accum)
{
	BufferTransferer btransf;
	for(auto& t: task_pool) {
		t.accumulate_result(d.get(), mem_props,
			btransf, num_points, accum);
	}
}

void ShadowProcessor::dump_vtk(const char* fname, double *result)
{
	std::ofstream fd(fname);

	fd << "# vtk DataFile Version 3.0\n"
		"Daylight solar incidence\n"
		"ASCII\n"
		"DATASET POLYDATA\n"
		"POINTS " << num_points << " float\n";

	MeshBuffers &mesh = task_pool[0].get_mesh();

	{
		MemMapper map{d.get(), mesh.vertex.mem.get()};

		auto ptr = map.get<VertexData*>();
		for(uint32_t i = 0; i < num_points; ++i) {
			fd << ptr[i].position.x << ' '
				<< ptr[i].position.y << ' '
				<< ptr[i].position.z << '\n';
		}
	}

	uint32_t face_count = mesh.idx_count / 3;
	fd << "POLYGONS " << face_count << ' ' << face_count * 4 << '\n';
	{
		MemMapper map{d.get(), mesh.index.mem.get()};

		auto ptr = map.get<uint32_t*>();
		for(uint32_t i = 0; i < face_count; ++i) {
			fd << '3';
			for(uint8_t j = 0; j < 3; ++j) {
				fd << ' ' << *ptr++;
			}
			fd << '\n';
		}
	}

	fd << "POINT_DATA " << num_points << "\n"
		"SCALARS incidence float 1\n"
		"LOOKUP_TABLE default\n";

	for(uint32_t i = 0; i < num_points; ++i) {
		fd << result[i] << '\n';
	}
}
