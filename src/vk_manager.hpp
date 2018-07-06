#pragma once

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

void chk_vk(VkResult err)
{
    	if(err != VK_SUCCESS) {
	    throw VulkanCreationError{err};
	}
}

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
