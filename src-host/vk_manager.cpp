#include "vk_manager.hpp"

void chk_vk(VkResult err)
{
	if(err != VK_SUCCESS) {
		throw VulkanCreationError{err};
	}
}

UVkCommandBuffers::UVkCommandBuffers(
	VkDevice device, const VkCommandBufferAllocateInfo& info)
{
	auto ptr = new VkCommandBuffer[info.commandBufferCount];
	chk_vk(vkAllocateCommandBuffers(device, &info, ptr));

	// Store in the unique_ptr.
	bufs = decltype(bufs){ptr, Deleter{
		device,
		info.commandPool,
		info.commandBufferCount
	}};
}

void UVkCommandBuffers::Deleter::operator()(const VkCommandBuffer* bufs)
{
	vkFreeCommandBuffers(d, cp, count, bufs);
	delete[] bufs;
}
