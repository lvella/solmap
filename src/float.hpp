#pragma once

#include <glm/vec3.hpp>

#ifndef DOUBLE

// Single precision
using real = float;
using Vec3 = glm::vec3;

#else

// Double precision
using real = double;
using Vec3 = glm::dvec3;

#endif
