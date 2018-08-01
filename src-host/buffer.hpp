#pragma once

#include "vk_manager.hpp"

enum BufferAccessDirection {
	HOST_WILL_WRITE_BIT = (1 << 0),
	HOST_WILL_READ_BIT = (1 << 1)
};

uint32_t find_memory_heap(
	const VkPhysicalDeviceMemoryProperties& mem_props,
	uint32_t allowed,
	VkMemoryPropertyFlags required,
	VkMemoryPropertyFlags prefered);

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

	VkDeviceMemory get_visible_mem()
	{
		if(staging_buf) {
			return staging_buf->mem.get();
		}
		return mem.get();
	}
};

// Writes to a buffer with local device access.
class BufferTransferer
{
public:
	BufferTransferer(VkDevice device,
		const VkPhysicalDeviceMemoryProperties& mem_props,
		VkCommandPool cmd_pool, VkQueue queue):
		d{device},
		mem_props{mem_props},
		cp{cmd_pool},
		q{queue}
	{}

	template <typename T, typename F>
	void indirect_transfer(const VkBuffer buf, uint32_t count,
		BufferAccessDirection direction, const F& func)
	{
		const uint32_t size = count *
			sizeof(typename std::remove_pointer<T>::type);

		// If our temporary buffer is not big enough,
		// allocate it.
		if(tmp_size < size) {
			tmp = std::make_unique<Buffer>(d,
				mem_props,
				VK_BUFFER_USAGE_TRANSFER_SRC_BIT
				| VK_BUFFER_USAGE_TRANSFER_DST_BIT,
				size, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
				0
			);
			tmp_size = size;
		}
		assert(tmp);

		// If command buffer is not allocated, do it.
		if(!cb) {
			cb = UVkCommandBuffers{d, VkCommandBufferAllocateInfo{
				VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
				nullptr,
				cp,
				VK_COMMAND_BUFFER_LEVEL_PRIMARY,
				1
			}};
		}


		// Range to flush and invalidate.
		const VkMappedMemoryRange range {
			VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
			nullptr,
			tmp->mem.get(),
			0,
			size
		};

		// Buffer region to copy.
		const VkBufferCopy region {
			0, 0, size
		};

		// Plain command buffer to record.
		const VkCommandBufferBeginInfo cbbi {
			VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			nullptr,
			0,
			nullptr
		};

		// Just submit, without synchronization.
		const VkSubmitInfo si{
			VK_STRUCTURE_TYPE_SUBMIT_INFO,
			nullptr,
			0,
			nullptr,
			nullptr,
			1,
			&cb[0],
			0,
			nullptr
		};

		// Record and execute command buffer to copy
		// the memory from the device:
		MemMapper map{d, tmp->mem.get()};
		if(direction & HOST_WILL_READ_BIT) {
			// Record the command buffer:
			chk_vk(vkBeginCommandBuffer(cb[0], &cbbi));
			vkCmdCopyBuffer(cb[0], buf, tmp->buf.get(), 1, &region);
			chk_vk(vkEndCommandBuffer(cb[0]));

			// Dispatch it:
			chk_vk(vkQueueSubmit(q, 1, &si, nullptr));

			// Wait for it to finish.
			chk_vk(vkQueueWaitIdle(q));

			// Retrieve the data to host readable memory.
			chk_vk(vkInvalidateMappedMemoryRanges(d, 1, &range));
		}

		// Execute the operation over the memory:
		{
			func(map.get<T>());
		}

		// Record and execute the command buffer to
		// copy from host to device:
		if(direction & HOST_WILL_WRITE_BIT) {
			// Flush the mapped region:
			chk_vk(vkFlushMappedMemoryRanges(d, 1, &range));

			// Record the command buffer:
			chk_vk(vkBeginCommandBuffer(cb[0], &cbbi));
			vkCmdCopyBuffer(cb[0], tmp->buf.get(), buf, 1, &region);
			chk_vk(vkEndCommandBuffer(cb[0]));

			// Dispatch it:
			chk_vk(vkQueueSubmit(q, 1, &si, nullptr));

			// Wait for it to finish.
			chk_vk(vkQueueWaitIdle(q));
		}
	}

	// Uses the best transfer method for the AccessibleBuffer
	template <typename T, typename F>
	void transfer(const AccessibleBuffer& buf, uint32_t count,
		BufferAccessDirection direction, const F& func)
	{
		if(buf.is_host_visible) {
			MemMapper map{d, buf.mem.get()};
			func(map.get<T>());
		} else {
			indirect_transfer<T>(buf.buf.get(), count,
				direction, func);
		}
	}

private:
	VkDevice d;
	const VkPhysicalDeviceMemoryProperties& mem_props;
	VkCommandPool cp;
	VkQueue q;

	UVkCommandBuffers cb;
	std::unique_ptr<Buffer> tmp;
	uint32_t tmp_size = 0;
};
