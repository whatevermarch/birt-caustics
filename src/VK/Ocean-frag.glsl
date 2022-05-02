#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

precision highp float;

#define USE_PUNCTUAL

//--------------------------------------------------------------------------------------
//  PS Inputs
//--------------------------------------------------------------------------------------

// maunually define UV output
#ifndef ID_TEXCOORD_0
#define ID_TEXCOORD_0
#endif

#include "GLTF_VS2PS_IO.glsl"

layout (location = 0) in VS2PS Input;

//--------------------------------------------------------------------------------------
//  PS Outputs
//--------------------------------------------------------------------------------------

#ifdef HAS_MOTION_VECTORS_RT
    layout(location = HAS_MOTION_VECTORS_RT) out vec2 Output_motionVect;
#endif

#ifdef HAS_FORWARD_RT
    layout (location = HAS_FORWARD_RT) out vec4 Output_finalColor;
#endif

#ifdef HAS_EMISSIVE_FLUX_RT
    layout (location = HAS_EMISSIVE_FLUX_RT) out vec4 Output_emissiveColor;
#endif

#ifdef HAS_SPECULAR_ROUGHNESS_RT
    layout (location = HAS_SPECULAR_ROUGHNESS_RT) out vec4 Output_specularRoughness;
#endif

#ifdef HAS_DIFFUSE_RT
    layout (location = HAS_DIFFUSE_RT) out vec4 Output_diffuseColor;
#endif

#ifdef HAS_NORMALS_RT
    layout (location = HAS_NORMALS_RT) out vec4 Output_normal;
#endif

#ifdef HAS_WORLD_COORD_RT
    layout (location = HAS_WORLD_COORD_RT) out vec4 Output_worldCoord;
#endif

//--------------------------------------------------------------------------------------
//  Uniforms
//--------------------------------------------------------------------------------------

layout (push_constant) uniform pushConstants
{
    uint iter;
    int rsmLightIndex;
} u_push;

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

layout (binding = ID_NormalMapArray) uniform sampler2DArray u_normalMaps;

//--------------------------------------------------------------------------------------
//  Main
//--------------------------------------------------------------------------------------

#include "functions.glsl"
//#include "PBRLighting.h"

// https://github.com/KhronosGroup/glTF/blob/master/extensions/2.0/Khronos/KHR_lights_punctual/README.md#range-property
float getRangeAttenuation(float range, float distance)
{
    if (range < 0.0)
    {
        // negative range means unlimited
        return 1.0;
    }
    return max(mix(1, 0, distance / range), 0);//max(min(1.0 - pow(distance / range, 4.0), 1.0), 0.0) / pow(distance, 2.0);
}

// https://github.com/KhronosGroup/glTF/blob/master/extensions/2.0/Khronos/KHR_lights_punctual/README.md#inner-and-outer-cone-angles
float getSpotAttenuation(vec3 pointToLight, vec3 spotDirection, float outerConeCos, float innerConeCos)
{
    float actualCos = dot(normalize(spotDirection), normalize(-pointToLight));
    if (actualCos > outerConeCos)
    {
        if (actualCos < innerConeCos)
        {
            return smoothstep(outerConeCos, innerConeCos, actualCos);
        }
        return 1.0;
    }
    return 0.0;
}

vec3 getDirectionalLightFlux(Light light)
{
    return light.intensity * light.color;
}

vec3 getPointLightFlux(Light light, vec3 pointToLight)
{
    float distance = length(pointToLight);
    float attenuation = getRangeAttenuation(light.range, distance);
    return attenuation * light.intensity * light.color;
}

vec3 getSpotLightFlux(Light light, vec3 pointToLight)
{
    float distance = length(pointToLight);
    float rangeAttenuation = getRangeAttenuation(light.range, distance);
    float spotAttenuation = getSpotAttenuation(pointToLight, -light.direction, light.outerConeCos, light.innerConeCos);
    return rangeAttenuation * spotAttenuation * light.intensity * light.color;
}

vec4 getNormal(vec2 uv)
{
    // In Blender, up-axis is z
    vec3 normal = texture(u_normalMaps, vec3(uv * 3.0f, float(u_push.iter))).rbg;
    normal.z = 1.0f - normal.z;

    return vec4(normal, 1.0f);
}

void main()
{
    // define water color
    const vec3 waterColor = vec3(0.18f, 0.4f, 0.69f); // deep blue sea
    const float alpha = 1.0f;

#ifdef HAS_MOTION_VECTORS_RT 
    Output_motionVect = Input.CurrPosition.xy / Input.CurrPosition.w -
                        Input.PrevPosition.xy / Input.PrevPosition.w;
#endif

#ifdef HAS_FORWARD_RT
    Output_finalColor = vec4(0.7); // dummy until you know what to do
#endif

#ifdef HAS_EMISSIVE_FLUX_RT
    // for screen G-Buffer => output 'emissive' 
    if (u_push.rsmLightIndex < 0)
    {
        Output_emissiveColor = vec4(vec3(0), alpha);
    }
    // for light RSM => output 'flux' instead
    else 
    {
        Light light = u_params.rsmLight;
        vec3 flux = vec3(1);
        
        //  now RSM supports only Directional Light and Spotlight
        int lightType = u_params.rsmLight.type;
        if (lightType == LightType_Directional)
        {
            flux *= getDirectionalLightFlux(u_params.rsmLight);
        }
        else if (lightType == LightType_Spot)
        {
            vec3 pointToLight = u_params.rsmLight.position - Input.WorldPos;
            // workaround: area of pixel in view-space will be calculated on Photon Tracing part instead.
            const float actualCos = dot(normalize(-light.direction), normalize(-pointToLight));
            const float pointDistance = length(pointToLight);
            //const float actualSolidAngle = 2 * M_PI * sqrt(1 - light.outerConeCos * light.outerConeCos);
            const float solidAngle = 4 * M_PI; // assuming that a spotlight is just a point light with stencil solid angle
            flux *= getSpotLightFlux(u_params.rsmLight, pointToLight);
            flux = flux * actualCos / (pointDistance * pointDistance * solidAngle);
        }
        
        Output_emissiveColor = vec4(flux, alpha);
    }
#endif

#ifdef HAS_SPECULAR_ROUGHNESS_RT
    Output_specularRoughness = vec4(vec3(0), 0.005f);
#endif

#ifdef HAS_DIFFUSE_RT
    Output_diffuseColor = vec4(vec3(0), 1.0f);
#endif

#ifdef HAS_NORMALS_RT
    Output_normal = getNormal(Input.UV0);
#endif

#ifdef HAS_WORLD_COORD_RT
    Output_worldCoord = vec4(Input.WorldPos, 1);
#endif
}