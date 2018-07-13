#pragma once

#include <functional>
#include <sstream>
#include <memory>
#include <type_traits>

#include <vulkan/vulkan.h>

// Error thrown if the create_vk fails.
struct VulkanCreationError: public std::runtime_error
{
public:
    VulkanCreationError(VkResult err):
	std::runtime_error{err_msg(err)}
    {}

private:
    static std::string err_msg(VkResult err)
    {
	std::stringstream ss;
	ss << "Error creating Vulkan object: error code " << err << '.';
	return ss.str();
    }
};

void chk_vk(VkResult err);

// Memory mapping guard. Flushes and unmap when destroyed.
class MemMapper
{
public:
    MemMapper(
	    VkDevice device, VkDeviceMemory memory,
	    VkDeviceSize offset=0, VkDeviceSize size=VK_WHOLE_SIZE
    ):
	d(device),
	m(memory),
	o(offset)
    {
	// Map the memory range.
	chk_vk(vkMapMemory(d, m, offset, size, 0, &data));
    }

    ~MemMapper()
    {
	// Flush the mapped range.
	VkMappedMemoryRange range {
	    VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
	    nullptr,
	    m,
	    o,
	    VK_WHOLE_SIZE
	};
	vkFlushMappedMemoryRanges(d, 1, &range);

	// Unmap it.
	vkUnmapMemory(d, m);
    }

    template<typename T>
    T get()
    {
	return static_cast<T>(data);
    }

private:
    VkDevice d;
    VkDeviceMemory m;
    VkDeviceSize o;
    void *data;
};

// Find the last argument type of a function type.
// Adapted from https://stackoverflow.com/a/46560993/578749
template<typename T>
struct tag
{
    using type = T;
};

template<typename F>
struct select_last;

template<typename R, typename... Ts>
struct select_last<R(*)(Ts...)>
{
    using type = typename decltype((tag<Ts>{}, ...))::type;
};

// Creates a Vulkan object, and return it inside an unique_ptr.
// Throws VulkanCreationError if VkResult is not VK_SUCCESS.
template <auto CreateFn, auto DestroyFn, typename CreateInfo, typename... Args>
auto create_vk(const CreateInfo& info, Args... args)
{
	// Get the type created by CreateFn, which is the base type of
	// of the last parameter: a pointer to the created object.
	using T = typename std::remove_pointer<
	    typename select_last<decltype(CreateFn)>::type
	>::type;

	// Create the object.
	T obj;
	chk_vk(CreateFn(args..., &info, nullptr, &obj));

	// By Vulkan specification, T is a pointer, so we manage it
	// with std::unique_ptr. We create a deleter class for this
	// case.
	struct Deleter {
		void operator()(T obj) {
			DestroyFn(obj, nullptr);
		}
	};
	return std::unique_ptr<
	    typename std::remove_pointer<T>::type,
	    Deleter
	>{obj};
}

// Same as before, but destructor takes an optional parameter.
template <auto CreateFn, auto DestroyFn,
    typename Param, typename CreateInfo>
auto create_vk_with_destroy_param(Param param,
	const CreateInfo& info)
{
	// Get the type created by CreateFn, which is the base type of
	// of the last parameter: a pointer to the created object.
	using T = typename std::remove_pointer<
	    typename select_last<decltype(CreateFn)>::type
	>::type;

	// Create the object.
	T obj;
	chk_vk(CreateFn(param, &info, nullptr, &obj));

	// By Vulkan specification, T is a pointer, so we manage it
	// with std::unique_ptr.
	struct Deleter {
		void operator()(T obj) {
			DestroyFn(param, obj, nullptr);
		}

		Param param;
	};

	return std::unique_ptr<
	    typename std::remove_pointer<T>::type,
	    Deleter
	>{obj, Deleter{param}};
}

// Naming the managed types, for convenience.
using UVkInstance = decltype(
	create_vk<vkCreateInstance, vkDestroyInstance>(VkInstanceCreateInfo{})
);

using UVkDevice = decltype(
	create_vk<
	    vkCreateDevice,
	    vkDestroyDevice
	>(VkDeviceCreateInfo{}, VkPhysicalDevice{})
);

using UVkBuffer = decltype(
	create_vk_with_destroy_param<
	    vkCreateBuffer,
	    vkDestroyBuffer
	>(VkDevice{}, VkBufferCreateInfo{})
);

using UVkDeviceMemory = decltype(
	create_vk_with_destroy_param<
	    vkAllocateMemory,
	    vkFreeMemory
	>(VkDevice{}, VkMemoryAllocateInfo{})
);
