#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_ARB_compute_shader  : enable

//--------------------------------------------------------------------------------------
//  CS workgroup definition
//--------------------------------------------------------------------------------------

layout (local_size_x = 16, local_size_y = 16) in;

//--------------------------------------------------------------------------------------
//  uniform data
//  set 0 : input data
//--------------------------------------------------------------------------------------

layout (push_constant) uniform pushConstants
{
    layout (offset = 0) int seed;
};

const int rsmLightIndex = 0;

#include "TransformParams.glsl"

struct CausticsParams
{
    TransformParams camera;
    TransformParams lights[4]; // we have 4 quarters of RSM

    float samplingMapScale;
    float IOR;
    float rayThickness;
    float tMax;
};
layout (std140, binding = ID_Params) uniform Params 
{
    CausticsParams u_params;
};

layout (binding = ID_SamplingMap) uniform sampler2D u_samplingMap;

layout (binding = ID_RSMWorldCoord) uniform sampler2D u_rsmWorldCoord;
layout (binding = ID_RSMNormal) uniform sampler2D u_rsmNormal;
layout (binding = ID_RSMSpecular) uniform sampler2D u_rsmSpecular;
layout (binding = ID_RSMFlux) uniform sampler2D u_rsmFlux;

layout (binding = ID_RSMDepth_0) uniform sampler2D u_rsmDepth0;
layout (binding = ID_RSMDepth_1toN) uniform sampler2D u_rsmDepth1N;
float fetchRSMDepth(vec2 coord, int mipLevel)
{
    if (mipLevel == 0)
    {
        coord /= 2;
        vec2 bound = 0.5f - (vec2(1) / textureSize(u_rsmDepth0, 0)) / 2;
        if(coord.x >= bound.x)
            coord.x = bound.x;
        if(coord.y >= bound.y)
            coord.y = bound.y;
        return texture(u_rsmDepth0, coord).r;
    }
    else
        return textureLod(u_rsmDepth1N, coord, float(mipLevel - 1)).r;
}
ivec2 getRSMDepthSize(int mipLevel)
{
    return mipLevel == 0 ? textureSize(u_rsmDepth0, 0) :
        textureSize(u_rsmDepth1N, mipLevel - 1);
}

layout (binding = ID_GBufDepth_0) uniform sampler2D u_gbufDepth0;
layout (binding = ID_GBufDepth_1toN) uniform sampler2D u_gbufDepth1N;
float fetchGBufDepth(vec2 coord, int mipLevel)
{
    return mipLevel == 0 ? texture(u_gbufDepth0, coord).r :
        textureLod(u_gbufDepth1N, coord, float(mipLevel - 1)).r;
}
ivec2 getGBufDepthSize(int mipLevel)
{
    return mipLevel == 0 ? textureSize(u_gbufDepth0, 0) :
        textureSize(u_gbufDepth1N, mipLevel - 1);
}

layout (binding = ID_GBufNormal) uniform sampler2D u_gbufNormal;

layout (std140, binding = ID_HitPosIrradiance) buffer HitPositionAndPackedIrradiance
{
    vec4 out_hitPos_irradiance[];
};

//--------------------------------------------------------------------------------------
//  main function
//--------------------------------------------------------------------------------------

#include "functions.glsl"
#include "RGBEConversion.h"
#include "ImageSpaceRT.h"

const float epsilon = 1e-4;
//const float fluxAmplifier = 2.5f; // for cornell box
const float fluxAmplifier = 7.f; // for sponza

vec2 sampleNoise()
{
    const int offset_x[4] = {1, 0, -1, 0};
    const int offset_y[4] = {0, -1, 0, 1};

    vec2 coord = gl_LocalInvocationID.xy + 0.5f;

    const int seed_x = int(mod(seed, 4));
    coord.x += offset_x[seed_x];
    coord.y += offset_y[seed_x];
    const float noise_x = texture(u_samplingMap, coord).r;

    const int seed_y = int(mod(seed_x + 1, 4));
    coord.x += offset_x[seed_y];
    coord.y += offset_y[seed_y];
    const float noise_y = texture(u_samplingMap, coord).g;

    return (seed / 4 < 1) ? vec2(noise_x, noise_y) : vec2(noise_y, noise_x);
}

//  retrieve the sample point from RSM and construct ray payload
int retrieveSample(out vec3 origin, out vec3 direction, out vec3 power)
{
    //  retrieve sampling coordinate (texture space, not normalized yet)
    const vec2 localSamplingCoord = sampleNoise();
    const vec2 samplingCoord = (localSamplingCoord + gl_WorkGroupID.xy) * ivec2(gl_WorkGroupSize.xy * u_params.samplingMapScale);
    const ivec2 rsmDim = textureSize(u_rsmFlux, 0) / 2; // shadow map in 4 quarters 
    if (samplingCoord.x >= rsmDim.x || samplingCoord.y >= rsmDim.y)
        return 1; // out-of-bound coordinate

    //  normalize coordinate
    vec2 normSamplingCoord = samplingCoord / uvec2(rsmDim.x, rsmDim.y);

    //  sample ray payload
    // remember we are splitting the shadow map in 4 quarters 
    normSamplingCoord *= 0.5f;

    // offsets of the center of the shadow map atlas
    const float offsetsX[4] = { 0.0, 0.5, 0.0, 0.5 };
    const float offsetsY[4] = { 0.0, 0.0, 0.5, 0.5 };
    normSamplingCoord += vec2(offsetsX[rsmLightIndex], offsetsY[rsmLightIndex]);

    const vec3 worldPos = texture(u_rsmWorldCoord, normSamplingCoord).rgb;
    const vec3 normal = texture(u_rsmNormal, normSamplingCoord).rgb * 2.0f - 1.0f;
    const vec4 specularRoughness = texture(u_rsmSpecular, normSamplingCoord).rgba;
    const vec4 fluxAlpha = texture(u_rsmFlux, normSamplingCoord).rgba;

    //  setting roughness cutoff to 30% (perceptualRoughness > 0.3 shall not pass)
    if (specularRoughness.a > 0.3f)
        return 2; // too much roughness for reflective/refractive bouncing

    //  filter negligible photon with too low flux power
    if (getPerceivedBrightness(fluxAlpha.xyz) < 0.04f) // ref. f0 value from 'PixelParams.glsl'
        return 3;

    //  determine the direction of the ray be Fresnel's equation
    vec3 bounceDir;

    const vec3 view = normalize(u_params.lights[rsmLightIndex].position.xyz - worldPos);
    const float distanceFromLight = length(u_params.lights[rsmLightIndex].position.xyz - worldPos);

    const float iorRatio = 1.0f / u_params.IOR; // air -> water
    const vec3 refracted = refract(-view, normal, iorRatio);

    // In the case of total internal reflection the refract() function
    // returns (0,0,0) and Fresnel has to be 1.
    float fresnel = 1.0f;
    if (length(refracted) >= epsilon) 
    {
        // Compute Fresnel using the Fresnel equations (not the Schlick
        // approximation!) This is more precise, especially if the
        // difference between the two indices of
        // refraction is very small, and avoids a special case
        const float cosIncident = dot(normal, view);
        const float cosTransmitted = -dot(refracted, normal);

        fresnel = fresnelUnpolarized(cosIncident, cosTransmitted, 1.0f, u_params.IOR);
    }

    // random and see if we go for reflection or refraction
    if (randFromCoord(samplingCoord) < fresnel) // reflection
        bounceDir = reflect(-view, normal);
    else // refraction
        bounceDir = refracted;

    origin = worldPos;
    direction = bounceDir;
    // workaround: area of pixel in view-space will be calculated on Photon Tracing part instead.
    const float pixelArea = (4 * distanceFromLight * distanceFromLight) / (rsmDim.x * rsmDim.y * 
                                        u_params.lights[rsmLightIndex].invTanHalfFovH * u_params.lights[rsmLightIndex].invTanHalfFovV);
    power = fluxAlpha.xyz * pixelArea;
    // workaround: compensate the intensity, since PBR shader overpowers the intensity
    power *= u_params.samplingMapScale * u_params.samplingMapScale * fluxAmplifier;

    return 0;
}

//  trace the ray through the depth map and decay the input power in-place. 
//  return the hitpoint (transformed w/ z = depth), along with normalized power (irradiance).
vec4 trace(vec3 origin, vec3 direction, vec3 power)
{
    vec3 hitPos = vec3(0);
    float irradiance_32b = uintBitsToFloat(0);
    
    vec3 lastPos = origin;
    float lastT = 0;
    vec2 lastCoord = vec2(0);
    bool bHit = false;
    
    //  trace on RSM first
    //  note : we ignore bHit and lastCoord from RSM tracing, 
    //         because we don't rule out hitting in this pass.
    float t;
    traceOnView(lastPos, direction, u_params.lights[rsmLightIndex], false, u_params.tMax, t, lastCoord);
    //if(u_params.tMax < 0) t = 0;
    lastPos = lastPos + direction * t;
    lastT += t;

    // continue tracing in screen space
    bHit = traceOnView(lastPos, direction, u_params.camera, true, u_params.tMax, t, lastCoord);
    lastPos = lastPos + direction * t;
    lastT += t;

    //  calculate irradiance
    if (bHit)
    {
        //  save hitpoint
        const vec4 viewPos = u_params.camera.view * vec4(lastPos, 1.0f);

        //  check normal and depth consistency
        vec3 visibleNormal = texture(u_gbufNormal, lastCoord).rgb * 2.0f - 1.0f;
        float visibleDepth = toViewDepth(fetchGBufDepth(lastCoord, 0), 
                                u_params.camera.nearPlane, u_params.camera.farPlane);
        
        AngularInfo angularInfo = getAngularInfo(-direction, visibleNormal, -viewPos.xyz);
        //const float NdotL = dot(-direction, visibleNormal);
        if (/*NdotL >= -epsilon*/ angularInfo.NdotL > 0 &&
            abs(viewPos.z - visibleDepth) <= u_params.rayThickness) 
        {
            const vec2 projPos = vec2(2, -2) * (lastCoord - 0.5f);
            const float projConstA = u_params.camera.farPlane / (u_params.camera.nearPlane - u_params.camera.farPlane);
            const float projConstB = projConstA * u_params.camera.nearPlane;
            const float projDepth = -projConstA + projConstB / (-viewPos.z);

            //  this finally guarantee that the ray hits, and the photon is visible to the camera
            hitPos = vec3(projPos.x, projPos.y, projDepth);

            //  calculate irradiance
            const ivec2 screenSize = textureSize(u_gbufNormal, 0);
            const float distanceFromEye = length(viewPos.xyz);
            const float invPixelArea = screenSize.x * screenSize.y * 
                                        u_params.camera.invTanHalfFovH * u_params.camera.invTanHalfFovV / 
                                        (4 * distanceFromEye * distanceFromEye);

            //  assuming that the receiver surface is diffuse, 
            //  apply lambertian BRDF and angle attenuation (due to rendering equation).
            const vec3 irradiance = power * invPixelArea * angularInfo.NdotL / M_PI; 

            //  compress irradiance
            //irradiance_32b = uintBitsToFloat(float3ToRGBE(irradiance));
            //  workaround: there's smth wrong with RGBE conversion
            irradiance_32b = getPerceivedBrightness(irradiance);
        }
    }

    return vec4(hitPos, irradiance_32b);
}

void main()
{
    const uint threadIdx_linear = gl_LocalInvocationIndex;
    const uint blockIdx_linear = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
    const uint writeIdx = blockIdx_linear * (gl_WorkGroupSize.x * gl_WorkGroupSize.y) + threadIdx_linear;

    //  get the sampling point first
    vec3 origin;
    vec3 direction;
    vec3 power;
    if (retrieveSample(origin, direction, power) != 0)
    {
        out_hitPos_irradiance[writeIdx] = vec4(0);
        return;
    }

    //  trace through depth map
    vec4 hitPos = trace(origin, direction, power);
    
    out_hitPos_irradiance[writeIdx] = hitPos;
}
