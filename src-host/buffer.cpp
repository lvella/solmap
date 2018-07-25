#include <iostream>
#include "buffer.hpp"

uint32_t find_memory_heap(
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

AccessibleBuffer::AccessibleBuffer(
	VkDevice d, const VkPhysicalDeviceMemoryProperties& mem_props,
	VkBufferUsageFlags usage, uint32_t size,
	BufferAccessDirection host_direction)
{
	try {
		// Try to create both host visible and
		// device local buffer:
		*static_cast<Buffer*>(this) = Buffer{
			d, mem_props, usage, size,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
			| VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0};
		is_host_visible = true;
	} catch(std::exception &e) {
		// Not possible, we must change the usage bits
		// to include transfer in the desired direction:
		if(host_direction & HOST_WILL_WRITE_BIT) {
			usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		}
		if(host_direction & HOST_WILL_READ_BIT) {
			usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		}

		*static_cast<Buffer*>(this) = Buffer{
			d, mem_props, usage, size,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0};
		is_host_visible = false;
	}
}

MaybeStagedBuffer::MaybeStagedBuffer(VkDevice device,
	const VkPhysicalDeviceMemoryProperties& mem_props,
	VkBufferUsageFlags usage, uint32_t size,
	BufferAccessDirection host_direction)
{
	AccessibleBuffer ab{device, mem_props, usage, size,
		host_direction};

	if(!ab.is_host_visible) {
		VkBufferUsageFlags staging_usage = 0;
		if(host_direction & HOST_WILL_WRITE_BIT) {
			staging_usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		}
		if(host_direction & HOST_WILL_READ_BIT) {
			staging_usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		}

		staging_buf = std::make_unique<Buffer>(device,
			mem_props, staging_usage, size,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0);
	}

	*static_cast<Buffer*>(this) = static_cast<Buffer>(std::move(ab));
}

