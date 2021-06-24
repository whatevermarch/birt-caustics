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

// g-buffer sampler
layout (set = 2, binding = 0) uniform sampler2D gcam_WorldCoord;
layout (set = 2, binding = 1) uniform sampler2D gcam_Normal;
layout (set = 2, binding = 2) uniform sampler2D gcam_Diffuse;
layout (set = 2, binding = 3) uniform sampler2D gcam_Specular;
layout (set = 2, binding = 4) uniform sampler2D gcam_Emissive;

//--------------------------------------------------------------------------------------
//  FS outputs
//--------------------------------------------------------------------------------------

layout (location = 0) out vec4 out_color;

//--------------------------------------------------------------------------------------
//  main function
//--------------------------------------------------------------------------------------

#include "functions.glsl"
#include "PBRLighting.h"

void main()
{
    //  invert screen's UV coord
    vec2 screenCoord = vec2(UV.x, 1.0f - UV.y);

    vec4 cam_WorldPos = texture(gcam_WorldCoord, screenCoord).rgba;
    vec4 cam_NormalAO = texture(gcam_Normal, screenCoord).rgba;
    vec3 cam_Diffuse = texture(gcam_Diffuse, screenCoord).rgb;
    vec4 cam_SpecularRoughness = texture(gcam_Specular, screenCoord).rgba;
    vec4 cam_EmissiveAlpha = texture(gcam_Emissive, screenCoord).rgba;

    vec3 cam_Normal = cam_NormalAO.rgb * 2.0f - 1.0f;
    float cam_AO = cam_NormalAO.a;
    vec3 cam_Specular = cam_SpecularRoughness.xyz;
    float cam_PerceptualRoughness = cam_SpecularRoughness.w;
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