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

// layout (push_constant) uniform pushConstants
// {
//     layout (offset = 0) int rsmLightIndex;
// };
const int rsmLightIndex = 0;

struct CausticsParams
{
    mat4 camViewProj;
    vec4 camPosition;

    float invTanHalfFovH;
    float invTanHalfFovV;

    uint rsmWidth;
    uint rsmHeight;

    vec4 lightPos[4]; // we have 4 quarters of RSM
};
layout (std140, binding = ID_Params) uniform Params 
{
    CausticsParams u_params;
};

layout (binding = ID_SamplingMap) uniform sampler2D u_samplingMap;
const uint samplingMapScale = 2;

layout (binding = ID_RSMWorldCoord) uniform sampler2D u_rsmWorldCoord;
layout (binding = ID_RSMNormal) uniform sampler2D u_rsmNormal;
layout (binding = ID_RSMSpecular) uniform sampler2D u_rsmSpecular;
layout (binding = ID_RSMFlux) uniform sampler2D u_rsmFlux;

layout (binding = ID_RSMDepth_0) uniform sampler2D u_rsmDepth0;
layout (binding = ID_RSMDepth_1toN) uniform sampler2D u_rsmDepth1N;

layout (binding = ID_GBufDepth_0) uniform sampler2D u_gbufDepth0;
layout (binding = ID_GBufDepth_1toN) uniform sampler2D u_gbufDepth1N;

layout (binding = ID_GBufNormal) uniform sampler2D u_gbufNormal;

layout (std140, binding = ID_HitPosIrradiance) buffer HitPositionAndPackedIrradiance
{
    vec4 out_hitPos_irradiance[];
};

//--------------------------------------------------------------------------------------
//  main function
//--------------------------------------------------------------------------------------

#include "functions.glsl"

const float IOR = 1.33;
const float criticalAngle = 49 * M_PI / 180;
const float epsilon = 1e-4;

//  random function
//  ref : https://thebookofshaders.com/10/
// float random (vec2 st) {
//     return fract(sin(dot(st, vec2(12.9898,78.233))) * 43758.5453123);
// }

//  encode vec3 color to RGBE 32-bit
// float encodeToRGBE(vec3 color)
// {

// }

//  retrieve the sample point from RSM and construct ray payload
int retrieveSample(out vec3 origin, out vec3 direction, out vec3 power)
{
    //  retrieve sampling coordinate (texture space, not normalized yet)
    vec2 samplingCoord = (texelFetch(u_samplingMap, ivec2(gl_LocalInvocationID.xy), 0).rg + gl_WorkGroupID.xy) * gl_WorkGroupSize.xy * samplingMapScale;
    if (samplingCoord.x >= u_params.rsmWidth || samplingCoord.y >= u_params.rsmHeight)
        return 1; // out-of-bound coordinate

    //  normalize coordinate
    samplingCoord /= uvec2(u_params.rsmWidth, u_params.rsmHeight);

    //  sample ray payload
    // remember we are splitting the shadow map in 4 quarters 
    samplingCoord *= 0.5f;

    // offsets of the center of the shadow map atlas
    const float offsetsX[4] = { 0.0, 0.5, 0.0, 0.5 };
    const float offsetsY[4] = { 0.0, 0.0, 0.5, 0.5 };
    samplingCoord += vec2(offsetsX[rsmLightIndex], offsetsY[rsmLightIndex]);

    vec3 worldPos = texture(u_rsmWorldCoord, samplingCoord).rgb;
    vec3 normal = texture(u_rsmNormal, samplingCoord).rgb * 2.0f - 1.0f;
    vec4 specularRoughness = texture(u_rsmSpecular, samplingCoord).rgba;
    vec4 fluxAlpha = texture(u_rsmFlux, samplingCoord).rgba;

    //  setting roughness cutoff to 30% (perceptualRoughness > 0.3 shall not pass)
    if (specularRoughness.a > 0.3f)
        return 2; // too much roughness for reflective/refractive bouncing

    //  filter negligible photon with too low flux power
    if (getPerceivedBrightness(fluxAlpha.xyz) < 0.04f) // ref. f0 value from 'PixelParams.glsl'
        return 3;

    //  determine the direction of the ray be Fresnel's equation
    vec3 view = normalize(u_params.lightPos[rsmLightIndex].xyz - worldPos);
    float incidentAngle = acos(clamp(dot(normal, view), 0.0f, 1.0f));

    //  for refracted rays
    //  setting alpha cutoff to 70% (transparency (1-alpha) < 0.3 shall be trated as reflection instead)
    vec3 bounceDir;
    if (fluxAlpha.w < 0.7f && incidentAngle < criticalAngle)
    {
        if (incidentAngle < epsilon)
            bounceDir = -normal;
        else
        {
            // ToDo : only works for light source above water. Fix it!!
            float sin_bounceAngle = sin(incidentAngle) * (1.0f / IOR);
            float cos_bounceAngle = sqrt(1.0f - sin_bounceAngle * sin_bounceAngle);

            vec3 bitangent = normalize(cross(cross(view, normal), normal));
            bounceDir = sin_bounceAngle * bitangent + cos_bounceAngle * (-normal);
        }
    }
    //  for reflected rays
    else
    {
        vec3 projView_normal = (dot(view, normal) / dot(normal, normal)) * normal;
        bounceDir = (projView_normal - view) * 2.0f + view;
    }

    origin = worldPos;
    //direction = bounceDir;
    //  ToDo : remove this. it is just to visualize first hit
    direction = -view;
    power = fluxAlpha.xyz;
}

//  trace the ray through the depth map and decay the input power in-place. 
//  return the hitpoint (transformed w/ z = depth), along with ray length (t) as w-elemeent.
vec4 trace(vec3 origin, vec3 direction, vec3 power)
{
    vec4 projHitPos = u_params.camViewProj * vec4(origin, 1.0f);

    //  calculate irradiance
    //float pixelArea = ...
    //vec3 irradiance /= pixelArea;
    vec3 irradiance = power;

    //  compress irradiance
    float irradiance_32b = uintBitsToFloat(packUnorm4x8(vec4(irradiance, 1))); // use uintBitsToFloat() to reinterpret_cast<>()

    return vec4(projHitPos.xyz / projHitPos.w, irradiance_32b);
}

void main()
{
    uint threadIdx_linear = gl_LocalInvocationIndex;
    uint blockIdx_linear = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
    uint writeIdx = blockIdx_linear * (gl_WorkGroupSize.x * gl_WorkGroupSize.y) + threadIdx_linear;

    //  get the sampling point first
    vec3 origin;
    vec3 direction;
    vec3 power;
    if (retrieveSample(origin, direction, power) != 0)
    {
        out_hitPos_irradiance[writeIdx] = vec4(0); // if unreachable -> ray length = -1.0
        return;
    }

    //  trace through depth map
    vec4 hitPos = trace(origin, direction, power);

    //  discard occluded photons
//    vec2 screenSampleCoord = hitPos.xy + vec2(0.5f);
//    screenSampleCoord.y = 1.0f - screenSampleCoord.y;
//    if (dot(-direction, texture(u_gbufNormal, screenSampleCoord).rgb) < 0)
//    {
//        out_hitPos_irradiance[writeIdx] = vec4(0); // if unreachable -> ray length = -1.0
//        return;
//    }

    out_hitPos_irradiance[writeIdx] = hitPos;
}
