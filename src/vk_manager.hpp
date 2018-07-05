#pragma once

#include <vulkan/vulkan.h>

template<typename T>
struct tag
{
    using type = T;
};

template<typename F>
struct select_last;

template<typename R, typename... Ts>
struct select_last<R(Ts...)>
{
    using type = typename decltype((tag<Ts>{}, ...))::type;
};

template<typename T, auto DestroyFn>
class UniqueVK
{
public:
    ~UniqueVK()
    {
	DestroyFn(obj, nullptr);
    }

    T& get()
    {
	return obj;
    }

private:
    T obj;
};

template <auto CreateFn, auto DestroyFn, typename CreateInfo, typename... Args>
auto create_vk(const CreateInfo& info, Args... args)
{
	using T = typename select_last<decltype(CreateFn)>::type;
	UniqueVK<T, DestroyFn> ret;

	CreateFn(args..., &info, nullptr, &ret.get());

	return ret;
}
