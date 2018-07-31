#pragma once

#include <vector>
#include <cstdint>

#include "float.hpp"

struct VertexData
{
	VertexData() = default;
	VertexData(const Vec3& p, const Vec3& n):
		position(p),
		normal(n)
	{}

	alignas(16) Vec3 position;
	alignas(16) Vec3 normal;
};

struct Mesh
{
	std::vector<VertexData> vertices;
	std::vector<uint32_t> indices;
};

Mesh load_scene(const char* filename);

void refine(Mesh& m, float max_length);
