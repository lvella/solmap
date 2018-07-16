#include <assimp/scene.h>

#include "vk_manager.hpp"
#include "shadow_processor.hpp"

static const uint32_t frame_size = 2048;

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
	VkBufferUsageFlags usage, uint32_t size):
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
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
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
		mesh->mNumVertices * 3 * sizeof(real)),
	index(device, mem_props,
		VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
		mesh->mNumFaces * 3 * sizeof(uint32_t)),
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
		real *ptr = map.get<real*>();
		for(uint32_t i = 0; i < mesh->mNumVertices; ++i) {
			for(uint32_t j = 0; j < 3; ++j) {
				ptr[i*3 + j] = factor * mesh->mVertices[i][j];
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
	qs{std::move(queues)}
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
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, // usage
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
}

void QueueFamilyManager::create_command_buffer(
	VkDevice d, VkRenderPass rp, VkPipeline pipeline)
{
	// Create the framebuffer:
	auto at = depth_image_view.get();
	framebuffer = UVkFramebuffer{VkFramebufferCreateInfo{
		VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		nullptr,
		0,
		rp,
		1,
		&at,
		frame_size,
		frame_size,
		1
	}, d};

	command_pool = UVkCommandPool{VkCommandPoolCreateInfo{
		VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		nullptr,
		0,
		qf_idx
	}, d};

	// Allocate a single command buffer:
	cmd_bufs = UVkCommandBuffers(d, VkCommandBufferAllocateInfo{
		VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		nullptr,
		command_pool.get(),
		VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		1
	});

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

	// End command buffer.
	chk_vk(vkEndCommandBuffer(cmd_bufs[0]));
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

	// Create framebuffers and command buffers:
	for(auto& qf: qfs) {
		qf.create_command_buffer(
			d.get(), render_pass.get(), pipeline.get());
	}
}

void ShadowProcessor::create_render_pipeline()
{
	// Create the vertex shader:
	static const uint32_t shader_data[] =
		#include "depth-map.vert.inc"
	;

	vert_shader = UVkShaderModule(VkShaderModuleCreateInfo {
		VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		nullptr,
		0,
		sizeof(shader_data),
		shader_data
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
		3 * sizeof(real),
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
	const VkPushConstantRange pcr {
		VK_SHADER_STAGE_VERTEX_BIT,
		0,
		sizeof(Vec4)
	};

	pipeline_layout = UVkPipelineLayout(VkPipelineLayoutCreateInfo{
		VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		nullptr,
		0,
		0,
		nullptr,
		1,
		&pcr
	}, d.get());

	// Depth buffer attachment:
	const VkAttachmentDescription dbad {
		0,
		VK_FORMAT_D32_SFLOAT,
		VK_SAMPLE_COUNT_1_BIT,
		VK_ATTACHMENT_LOAD_OP_CLEAR,
		VK_ATTACHMENT_STORE_OP_STORE,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		VK_ATTACHMENT_STORE_OP_DONT_CARE,
		VK_IMAGE_LAYOUT_UNDEFINED,
		// TODO: verify if this is correct:
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
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

	pipeline = UVkGraphicsPipeline(VkGraphicsPipelineCreateInfo{
		VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		nullptr,
		0,
		1,       // stageCount
		&pss,    // pStages
		&pvis,   // pVertexInputState
		&pias,   // pInputAssemblyState
		nullptr, // pTessellationState
		&pvs,    // pViewportState
		&prs,    // pRasterizationState
		&pms,    // pMultisampleState
		&pdss,   // pDepthStencilState
		nullptr, // pColorBlendState
		nullptr, // pDynamicState
		pipeline_layout.get(), // layout
		render_pass.get(),     // renderPass
		0,                     // subpass
		VK_NULL_HANDLE, // basePipelineHandle
		-1              // basePipelineIndex
	}, d.get(), nullptr, 1);
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
}
