#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

precision highp float;

//--------------------------------------------------------------------------------------
//  FS inputs
//--------------------------------------------------------------------------------------

layout (location = 0) in vec2 UV;

//--------------------------------------------------------------------------------------
//  uniform data
//  set 0 : uniform data
//  set 1 : RSM samplers
//  set 2 : G-BUffer input attachments
//--------------------------------------------------------------------------------------

#include "perFrameStruct.h"

layout (std140, set = 0, binding = ID_PER_FRAME) uniform perFrame 
{
    PerFrame myPerFrame;
};

// shadow (depth) sampler is declared in "shadowFiltering.h"
layout(set = 1, binding = ID_shadowMap) uniform sampler2DShadow u_shadowMap;
layout(set = 1, binding = ID_shadowMap + 1) uniform usampler2D u_opacityMap;
layout(set = 1, binding = ID_shadowMap + 2) uniform sampler2DShadow u_shadowMapBase;

// input_attachment_index = index declared in framebuffer
layout (input_attachment_index = 0, set = 2, binding = 0) uniform subpassInput gcam_WorldCoord;
layout (input_attachment_index = 1, set = 2, binding = 1) uniform subpassInput gcam_Normal;
layout (input_attachment_index = 2, set = 2, binding = 2) uniform subpassInput gcam_Diffuse;
layout (input_attachment_index = 3, set = 2, binding = 3) uniform subpassInput gcam_Specular;
layout (input_attachment_index = 4, set = 2, binding = 4) uniform subpassInput gcam_Emissive;

//--------------------------------------------------------------------------------------
//  FS outputs
//--------------------------------------------------------------------------------------

layout (location = 0) out vec4 out_color;

//--------------------------------------------------------------------------------------
//  main function
//--------------------------------------------------------------------------------------

#include "functions.glsl"
//#include "shadowFiltering.h"
#include "PBRLighting.h"

void main()
{
    //  load pixel data from g-buffer
    vec4 cam_WorldPos = subpassLoad(gcam_WorldCoord).rgba;
    vec4 cam_NormalAO = subpassLoad(gcam_Normal).rgba;
    vec3 cam_Normal = cam_NormalAO.rgb * 2.0 - 1.0;
    float cam_AO = cam_NormalAO.a;
    vec3 cam_Diffuse = subpassLoad(gcam_Diffuse).rgb;
    vec4 cam_SpecularRoughness = subpassLoad(gcam_Specular).rgba;
    vec3 cam_Specular = cam_SpecularRoughness.xyz;
    float cam_PerceptualRoughness = cam_SpecularRoughness.w;
    vec4 cam_EmissiveAlpha = subpassLoad(gcam_Emissive).rgba;
    vec3 cam_Emissive = cam_EmissiveAlpha.rgb;
    float cam_Alpha = cam_EmissiveAlpha.a;
    
    //  direct lighting
    out_color = vec4(doPbrLighting(
                        cam_WorldPos.xyz,
                        cam_Normal,
                        cam_Diffuse,
                        cam_Specular,
                        cam_PerceptualRoughness
                    ) * cam_AO + cam_Emissive, 
                    cam_Alpha);
}