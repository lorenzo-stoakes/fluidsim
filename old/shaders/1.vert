#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

layout (location = 0) in vec3 in_pos;
layout (location = 1) in vec3 in_colour;

layout (binding = 0) uniform UBO
{
	mat4 projection;
	mat4 model;
	mat4 view;
} ubo;

layout (location = 0) out vec3 out_colour;

out gl_PerVertex
{
    vec4 gl_Position;
};

void main()
{
	out_colour = in_colour;
	gl_Position = ubo.projection * ubo.view * ubo.model * vec4(in_pos.xyz, 1.0);
}
