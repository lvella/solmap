#include <assimp/scene.h>

#include "vk_manager.hpp"
#include "shadow_processor.hpp"

Buffer::Buffer(VkDevice d,
	const VkPhysicalDeviceMemoryProperties& mem_props,
	VkBufferUsageFlags usage, uint32_t size)
{
	buf = create_vk_with_destroy_param<vkCreateBuffer, vkDestroyBuffer>(
		d, VkBufferCreateInfo{
			VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			nullptr,
			0,
			size,
			usage,
			VK_SHARING_MODE_EXCLUSIVE,
			0,
			nullptr
		}
	);

	// Allocate the memory for the buffer from a suitable heap.
	VkMemoryRequirements mem_reqs;
	vkGetBufferMemoryRequirements(d, buf.get(), &mem_reqs);

	// Find a suitable buffer that the host can write:
	uint32_t mtype;
	uint32_t i = 0;
	for(; i < mem_props.memoryTypeCount; ++i)
	{
		// Is the memory type suitable?
		if (!(mem_reqs.memoryTypeBits & (1 << i))) {
			continue;
		}

		// Is the memory visible?
		if(!(mem_props.memoryTypes[i].propertyFlags
			& VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
		{
			continue;
		}

		// The memory is suitable, hold it.
		mtype = i;

		// The memory found is suitable, but is it local to
		// the device? If local, we stop immediatelly, because
		// we prefer local memory for performance.
		if((mem_props.memoryTypes[i].propertyFlags
			& VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
		{
			break;
		}
	}
	if(i == mem_props.memoryTypeCount) {
		throw std::runtime_error("No suitable memory type found.");
	}

	// Allocate the memory:
	mem = create_vk_with_destroy_param<vkAllocateMemory, vkFreeMemory>(
		d, VkMemoryAllocateInfo{
			VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			nullptr,
			mem_reqs.size,
			mtype
		}
	);

	// Associate it with the buffer:
	vkBindBufferMemory(d, buf.get(), mem.get(), 0);
}

MeshBuffers::MeshBuffers(VkDevice device,
	const VkPhysicalDeviceMemoryProperties& mem_props,
	const aiMesh* mesh
):
	vertex(device, mem_props,
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		mesh->mNumVertices * sizeof(Vec3)),
	index(device, mem_props,
		VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
		mesh->mNumFaces * 3 * sizeof(uint32_t))
{
	// TODO: fill buffers
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
	double c = cos(p.alt);

	Vec3 sun{
		std::sin(p.az) * c,
		std::sin(p.alt),
		-std::cos(p.az) * c
	};

	sum += sun;
	++count;
}
