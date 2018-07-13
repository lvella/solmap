#include "vk_manager.hpp"

void chk_vk(VkResult err)
{
    	if(err != VK_SUCCESS) {
	    throw VulkanCreationError{err};
	}
}
