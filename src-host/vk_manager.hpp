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

// Throws if parameter is different from success.
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

// Find the last argument type of a function pointer type.
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

// Find the first argument type of a function pointer type.
template<typename F>
struct select_first;

template<typename R, typename T, typename... Ts>
struct select_first<R(*)(T, Ts...)>
{
    using type = T;
};

// Base class for automatic manager of Vulkan objects.
// The destructor is provided by the derived class.
template <auto CreateFn>
class Manager
{
public:
    using ManagedType = typename std::remove_pointer<
	    typename select_last<decltype(CreateFn)>::type
	>::type;

    Manager() = default;

    ManagedType get() const
    {
	return obj;
    }

    operator bool () const
    {
	return bool{obj};
    }

    // Non-copyable:
    Manager(const Manager&) = delete;
    void operator=(const Manager&) = delete;

    // Movable
    Manager& operator=(Manager&& other)
    {
	obj = other.obj;
	other.obj = nullptr;
	return *this;
    }

protected:
    // Only constructable by base class:
    template<typename CreateInfo, typename... Args>
    Manager(const CreateInfo& info, Args... args)
    {
	// Create the object.
	chk_vk(CreateFn(args..., &info, nullptr, &obj));
    }

    Manager(Manager&& other)
    {
	*this = std::move(other);
    }

    ManagedType obj;
};


// Manages a Vulkan object, pretty much like an
// std::unique_ptr would do, but taking the creation
// info as reference, for convenience.
template <auto CreateFn, auto DestroyFn>
class ManagedVk:
    public Manager<CreateFn>
{
public:
    ManagedVk() = default;

    template<typename... Args>
    ManagedVk(Args... args):
	Manager<CreateFn>{args...}
    {}

    ManagedVk(ManagedVk&&) = default;
    ManagedVk& operator=(ManagedVk&&) = default;

    ~ManagedVk()
    {
	// I will not count on all Vulkan destroy functions
	// accepting nullptr as input, so I check if not null.
	if(this->obj) {
	    DestroyFn(this->obj, nullptr);
	}
    }
};

// Manages a Vulkan object whose destructor takes the
// same first argument as the constructor.
template <auto CreateFn, auto DestroyFn>
class ManagedDPVk:
    public Manager<CreateFn>
{
public:
    using DestroyParamType = typename select_first<decltype(DestroyFn)>::type;

    ManagedDPVk() = default;

    template<typename CreateInfo, typename... Args>
    ManagedDPVk(const CreateInfo& info, DestroyParamType dparam, Args... args):
	Manager<CreateFn>{info, dparam, args...},
	destroy_param(dparam)
    {}

    ManagedDPVk(ManagedDPVk&&) = default;
    ManagedDPVk& operator=(ManagedDPVk&&) = default;

    ~ManagedDPVk()
    {
	// I will not count on all Vulkan destroy functions
	// accepting nullptr as input, so I check if not null.
	if(this->obj) {
	    DestroyFn(destroy_param, this->obj, nullptr);
	}
    }

private:
    DestroyParamType destroy_param;
};

// Naming the managed types, for convenience.
using UVkInstance = ManagedVk<vkCreateInstance, vkDestroyInstance>;

using UVkDevice = ManagedVk<vkCreateDevice, vkDestroyDevice>;

using UVkBuffer = ManagedDPVk<vkCreateBuffer, vkDestroyBuffer>;

using UVkDeviceMemory = ManagedDPVk<vkAllocateMemory, vkFreeMemory>;

using UVkShaderModule = ManagedDPVk<
    vkCreateShaderModule,
    vkDestroyShaderModule
>;

using UVkPipelineLayout = ManagedDPVk<
    vkCreatePipelineLayout,
    vkDestroyPipelineLayout
>;

using UVkRenderPass = ManagedDPVk<vkCreateRenderPass, vkDestroyRenderPass>;

using UVkGraphicsPipeline = ManagedDPVk<
	vkCreateGraphicsPipelines,
	vkDestroyPipeline
>;

using UVkImage = ManagedDPVk<
	vkCreateImage,
	vkDestroyImage
>;
