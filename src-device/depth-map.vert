#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(binding = 0) uniform InputData
{
	// This orientation is given as a normalized quaternion,
	// where the scalar component is w.
	vec4 orientation;
};

layout(location = 0) in vec3 inPosition;

out gl_PerVertex {
	vec4 gl_Position;
};

#include "quaternion.glsl"

void main()
{
	vec3 pos = quat_rot_vec(orientation, inPosition);

	// For some silly reason, Vulkan decided to support D3D,
	// cliping range [0, 1], instead of the naturally
	// unscaled [-1, 1], requiring the folling transformation
	// on the output:
	gl_Position = vec4(pos.xy, pos.z * 0.5 + 0.5, 1.0);
}
