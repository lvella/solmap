#pragma once

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#ifndef DOUBLE

// Single precision
using real = float;
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;

#else

// Double precision
using real = double;
using Vec3 = glm::dvec3;
using Vec4 = glm::dvec4;

#endif
