#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

//--------------------------------------------------------------------------------------
//  VS Outputs
//--------------------------------------------------------------------------------------

// maunually define UV output
#ifndef ID_TEXCOORD_0
#define ID_TEXCOORD_0
#endif

#include "GLTF_VS2PS_IO.glsl"

layout (location = 0) out VS2PS Output;

//--------------------------------------------------------------------------------------
//  Uniforms
//--------------------------------------------------------------------------------------

#include "perFrameStruct.h"

struct OceanParams
{
    mat4 currWorld;
    mat4 prevWorld;
    mat4 currViewProj;
    mat4 prevViewProj;

	Light rsmLight;
};
layout (std140, binding = ID_Params) uniform Params 
{
    OceanParams u_params;
};

const vec3 positionList[4] = 
{
	vec3(-1, 0, -1),
	vec3(-1, 0, 1),
	vec3(1, 0, 1),
	vec3(1, 0, -1)
};

const vec2 uvList[4] = 
{
	vec2(0, 0),
	vec2(0, 1),
	vec2(1, 1),
	vec2(1, 0)
};

const uint indexList[6] = 
{
	0, 3, 1, 1, 3, 2
};

void main()
{
	const uint idx = indexList[gl_VertexIndex];
	const vec4 localPos = vec4(positionList[idx], 1);
	const vec4 worldPos = u_params.currWorld * localPos;

	gl_Position = u_params.currViewProj * worldPos;

	Output.WorldPos = worldPos.xyz / worldPos.w;
	Output.UV0 = uvList[idx];

#ifdef HAS_MOTION_VECTORS
	Output.CurrPosition = gl_Position; // current's frame vertex position 
	Output.PrevPosition = u_params.prevViewProj * u_params.prevWorld * localPos;
#endif
}