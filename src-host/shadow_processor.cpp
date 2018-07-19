#include <assimp/scene.h>
#include <glm/glm.hpp>

#include "vk_manager.hpp"
#include "shadow_processor.hpp"

static const uint32_t frame_size = 2048;

struct VertexDataInput
{
	Vec3 position;
	Vec3 normal;
};

struct UniformDataInput
{
	Vec4 orientation;
	Vec3 suns_direction;
};

// Get quaternion rotation from unit vector a to unit vector b.
// Doesn't work if a and b are opposites.
// Lets assume, for now, that it never happens.
static Vec4 rot_from_unit_a_to_unit_b(Vec3 a, Vec3 b)
{
	Vec4 ret{glm::cross(a,b), 1.0 + glm::dot(a, b)};
	return glm::normalize(ret);
}

static uint32_t find_memory_heap(
	const VkPhysicalDeviceMemoryProperties& mem_props,
	uint32_t allowed,
	VkMemoryPropertyFlags required,
	VkMemoryPropertyFlags prefered)
{
	// Find a suitable heap with the requested properties:
	uint32_t mtype = mem_props.memoryTypeCount;
	for(uint32_t i = 0; i < mem_props.memoryTypeCount; ++i)
	{
		// Is the memory type suitable?
		if (!(allowed & (1 << i))) {
			continue;
		}

		// Is the memory visible?
		if((mem_props.memoryTypes[i].propertyFlags
			& required) != required)
		{
			continue;
		}

		// The memory is suitable, hold it.
		mtype = i;

		// The memory found is suitable, but is it of the preferable
	       	// type? If so, we stop immediatelly, because we found the
		// best memory type.
		if((mem_props.memoryTypes[i].propertyFlags
			& prefered) == prefered)
		{
			break;
		}
	}
	if(mtype == mem_props.memoryTypeCount) {
		throw std::runtime_error("No suitable memory type found.");
	}
	return mtype;
}

Buffer::Buffer(VkDevice d,
	const VkPhysicalDeviceMemoryProperties& mem_props,
	VkBufferUsageFlags usage, uint32_t size,
	VkMemoryPropertyFlags required,
	VkMemoryPropertyFlags prefered
):
	buf{VkBufferCreateInfo{
			VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			nullptr,
			0,
			size,
			usage,
			VK_SHARING_MODE_EXCLUSIVE,
			0,
			nullptr
		}, d
	}
{
	// Allocate the memory for the buffer from a suitable heap.
	VkMemoryRequirements mem_reqs;
	vkGetBufferMemoryRequirements(d, buf.get(), &mem_reqs);

	// Find a suitable buffer that the host can write:
	uint32_t mtype = find_memory_heap(
		mem_props,
		mem_reqs.memoryTypeBits,
		required, prefered
	);

	// Allocate the memory:
	mem = UVkDeviceMemory{VkMemoryAllocateInfo{
			VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			nullptr,
			mem_reqs.size,
			mtype
		}, d
	};

	// Associate it with the buffer:
	vkBindBufferMemory(d, buf.get(), mem.get(), 0);
}

MeshBuffers::MeshBuffers(VkDevice device,
	const VkPhysicalDeviceMemoryProperties& mem_props,
	const aiMesh* mesh
):
	vertex(device, mem_props,
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		mesh->mNumVertices * sizeof(VertexDataInput),
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
	),
	index(device, mem_props,
		VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
		mesh->mNumFaces * 3 * sizeof(uint32_t),
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
	),
	idx_count(mesh->mNumFaces * 3)
{
	{
		// Copy the vertex data.
		// In the process, scale it so it will always stay
		// within the render area. Since we know all the points
		// are within [-1, 1], the biggest lenth possible is 2*sqrt(3)
		// (the diagonal of a cube with L = 2). Thus, by scalig by
		// 1/sqrt(3), we ensure que biggest lenght fits in the [-1, 1]
		// square of the render area.
		const real factor = 1.0 / std::sqrt(3.0);
		MemMapper map(device, vertex.mem.get());
		auto ptr = map.get<VertexDataInput*>();
		for(uint32_t i = 0; i < mesh->mNumVertices; ++i, ++ptr) {
			for(uint8_t j = 0; j < 3; ++j) {
				ptr[i].position[j] =
					factor * mesh->mVertices[i][j];
				ptr[i].normal[j] = mesh->mNormals[i][j];
			}
		}
	}

	{
		// Copy the index data.
		MemMapper map(device, index.mem.get());
		auto ptr = map.get<uint32_t (*)[3]>();
		for(uint32_t i = 0; i < mesh->mNumFaces; ++i) {
			assert(mesh->mFaces[i].mNumIndices == 3);
			for(uint32_t j = 0; j < 3; ++j) {
				ptr[i][j] = mesh->mFaces[i].mIndices[j];
			}
		}
	}
}

QueueFamilyManager::QueueFamilyManager(
	VkDevice device,
	const VkPhysicalDeviceMemoryProperties& mem_props,
	uint32_t idx,
	std::vector<VkQueue>&& queues,
	const aiScene* scene
):
	qf_idx{idx},
	qs{std::move(queues)},
	uniform_buf{device, mem_props,
		VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		sizeof(UniformDataInput),
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
	},
	uniform_map{device, uniform_buf.mem.get()}
{
	// There may be multiple meshes in the loaded scene,
	// load them all.
	meshes.reserve(scene->mNumMeshes);
	for(unsigned i = 0; i < scene->mNumMeshes; ++i) {
		meshes.emplace_back(device, mem_props, scene->mMeshes[i]);
	}

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

	// Create the descriptor for the uniform.
	const VkDescriptorPoolSize dps[] = {
	       	{
			VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			1
		},
	       	{
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			1
		}
	};

	desc_pool = UVkDescriptorPool(VkDescriptorPoolCreateInfo{
		VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		nullptr,
		0,
		1,
		(sizeof dps) / (sizeof dps[0]),
		dps
	}, device);

	// Create the frame fence
	/*frame_fence = UVkFence(VkFenceCreateInfo{
		VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		nullptr,
		0
	}, device);*/
}

void QueueFamilyManager::create_command_buffer(const ShadowProcessor& sp)
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

	command_pool = UVkCommandPool{VkCommandPoolCreateInfo{
		VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		nullptr,
		0,
		qf_idx
	}, sp.d.get()};

	// Create descriptor set, for uniform variable
	const VkDescriptorSetLayout dset_layouts[] = {
		sp.uniform_desc_set_layout.get(),
		sp.comp_sampler_dset_layout.get()
	};
	const VkDescriptorSetAllocateInfo dsai {
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		nullptr,
		desc_pool.get(),
		(sizeof dset_layouts) / (sizeof dset_layouts[0]),
		dset_layouts
	};

	VkDescriptorSet dsets[2];
	chk_vk(vkAllocateDescriptorSets(sp.d.get(), &dsai, dsets));
	uniform_desc_set = dsets[0];
	img_sampler_desc_set = dsets[1];

	const VkDescriptorBufferInfo buffer_info {
		uniform_buf.buf.get(),
		0,
		VK_WHOLE_SIZE
	};
	
	const VkDescriptorImageInfo img_info {
		// sampler, unused because it is immutable, but set anyway:
		sp.depth_sampler.get(),
		depth_image_view.get(),
		VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL
	};

	const VkWriteDescriptorSet wds[] = {
	       	{
			VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			nullptr,
			uniform_desc_set,
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
			img_sampler_desc_set,
			1,
			0,
			1,
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			&img_info,
			nullptr,
			nullptr
		}
	};

	vkUpdateDescriptorSets(sp.d.get(), 2, wds, 0, nullptr);

	// Allocate a single command buffer:
	cmd_bufs = UVkCommandBuffers(sp.d.get(), VkCommandBufferAllocateInfo{
		VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		nullptr,
		command_pool.get(),
		VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		1
	});

	fill_command_buffer(
		sp.render_pass.get(),
		sp.graphic_pipeline.get(),
		sp.graphic_pipeline_layout.get()
	);
}

void QueueFamilyManager::fill_command_buffer(
	VkRenderPass rp, VkPipeline pipeline,
	VkPipelineLayout pipeline_layout)
{
	// Start recording the commands in the command buffer.
	VkCommandBufferBeginInfo cbbi{
		VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		nullptr,
		0, // VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT
		nullptr
	};
	chk_vk(vkBeginCommandBuffer(cmd_bufs[0], &cbbi));

	// Draw the depth buffer command:
	VkClearValue cv;
	cv.depthStencil = {1.0, 0};

	VkRenderPassBeginInfo rpbi {
		VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		nullptr,
		rp,
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
	vkCmdBindPipeline(cmd_bufs[0], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

	// Bind the vertex buffer
	std::vector<VkBuffer> vertexBuffers(meshes.size());
	for(size_t i = 0; i < meshes.size(); ++i) {
		vertexBuffers[i] = meshes[i].vertex.buf.get();
	}
	std::vector<VkDeviceSize> offsets(meshes.size(), 0);
	vkCmdBindVertexBuffers(cmd_bufs[0], 0, meshes.size(),
		vertexBuffers.data(), offsets.data());

	// Bind the uniform variable.
	vkCmdBindDescriptorSets(cmd_bufs[0],
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		pipeline_layout, 0, 1, &uniform_desc_set, 0, nullptr);

	// Draw things:
	const VkDeviceSize zero_offset = 0;
	for(auto& m: meshes) {
		// Bind vertex buffer.
		vkCmdBindVertexBuffers(cmd_bufs[0], 0, 1,
			&m.vertex.buf.get(), &zero_offset);

		// Bind index buffer.
		vkCmdBindIndexBuffer(cmd_bufs[0],
			m.index.buf.get(), 0, VK_INDEX_TYPE_UINT32);

		// Draw the object:
		vkCmdDrawIndexed(cmd_bufs[0], m.idx_count, 1, 0, 0, 0);
	}

	// End drawing stuff.
	vkCmdEndRenderPass(cmd_bufs[0]);

	// Begin incidence compute.
	// TODO...
	// End compute phase.

	// End command buffer.
	chk_vk(vkEndCommandBuffer(cmd_bufs[0]));
}

void QueueFamilyManager::render_frame(const Vec3& direction)
{
	// Get pointer to device memory:
	auto params = uniform_map.get<UniformDataInput*>();

	// The rotation from model space sun to (0, 0, -1),
	// which is pointing to the viewer in Vulkan coordinates.
	params->orientation = rot_from_unit_a_to_unit_b(
		direction, Vec3{0.0, 0.0, -1.0});

	// Set sun's direction:
	params->suns_direction = direction;

	// Flush the copy.
	uniform_map.flush();

	VkSubmitInfo si{
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
	vkQueueSubmit(qs[0], 1, &si, VK_NULL_HANDLE);

	vkQueueWaitIdle(qs[0]);
}

ShadowProcessor::ShadowProcessor(
	VkPhysicalDevice pdevice,
	UVkDevice&& device,
	std::vector<std::pair<uint32_t, std::vector<VkQueue>>>&& queues,
	const aiScene* scene
):
	d{std::move(device)}
{
	// Get the memory properties needed to allocate the buffer.
	VkPhysicalDeviceMemoryProperties mem_props;
	vkGetPhysicalDeviceMemoryProperties(pdevice, &mem_props);

	// Create one queue family manager per queue family,
	// the buffers will be local per queue family.
	qfs.reserve(queues.size());
	for(auto& q: queues)
	{
		qfs.emplace_back(d.get(), mem_props, q.first,
			std::move(q.second), scene);
	}

	// Create depth buffer rendering pipeline:
	create_render_pipeline();

	// Create solar incidence compute pipeline:
	create_compute_pipeline();

	// Create framebuffers and command buffers:
	for(auto& qf: qfs) {
		qf.create_command_buffer(*this);
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
		sizeof(VertexDataInput),
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

	const VkSubpassDependency sdep {
		0, // srcSubpass
		VK_SUBPASS_EXTERNAL, // dstSubpass
		VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, // srcStageMask
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, // dstStageMask
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, // srcAccessMask
		VK_ACCESS_SHADER_READ_BIT, // dstAccessMask
		0 // dependencyFlags
	};

	render_pass = UVkRenderPass(VkRenderPassCreateInfo {
		VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		nullptr,
		0,
		1,
		&dbad,
		1,
		&sd,
		0,
		nullptr
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
	// TODO: place here the correct number of points:
	const uint32_t num_points = 200;

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
			1,
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			1,
			VK_SHADER_STAGE_COMPUTE_BIT,
			&depth_sampler.get()
		},

		// Input points (same buffer as graphics attributes):
		{
			2,
			VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			num_points,
			VK_SHADER_STAGE_COMPUTE_BIT,
			nullptr
		},

		// Accumulated incidence for each input point:
		{
			3,
			VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			num_points,
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
	const VkSpecializationMapEntry map_entry {
		0,
		0,
		sizeof num_points
	};

	const VkSpecializationInfo sinfo {
		1,
		&map_entry,
		sizeof num_points,
		&num_points
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

	// TODO: use all queue families.
	qfs[0].render_frame(sun);

	sum += sun;
	++count;
}
