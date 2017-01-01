#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

layout (location = 0) in vec3 in_colour;
layout (location = 0) out vec4 out_frag_colour;

void main()
{
	out_frag_colour = vec4(in_colour, 1.0);
}
