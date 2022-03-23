#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

layout (location = 0) in vec4 in_pointColor;
layout (location = 1) in float in_valid;

layout (location = 0) out vec4 out_irradiance;

const float epsilon = 1e-8;

void main()
{
	if(in_valid < epsilon)
		discard;

	vec2 cxy = 2.0f * gl_PointCoord - 1.0f;
	float r = sqrt(dot(cxy, cxy));
	if (r > 1.0f) {
		discard;
	}

	//	ease-out function : quadratic
	float alpha = 1 - r;
	alpha *= alpha;

	out_irradiance = vec4(in_pointColor.xyz * alpha * 0.05f * in_valid, 1);
}