#pragma once

#include <vector>
#include <cstdint>

#include "float.hpp"

struct VertexData
{
	Vec3 position;
	float a;
	Vec3 normal;
	float b;
};

struct Mesh
{
	std::vector<VertexData> vertices;
	std::vector<uint32_t> indices;
};

Mesh load_scene(const char* filename);
