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

#include "TransformParams.glsl"

struct SSRParams
{
    TransformParams camera;
    
    float samplingMapScale;
    float IOR;
    float rayThickness;
    float tMax;
};
layout (std140, binding = ID_Params) uniform Params 
{
    SSRParams u_params;
};

layout (binding = ID_SamplingMap) uniform sampler2D u_samplingMap;

layout (binding = ID_GBufWorldCoord) uniform sampler2D u_gbufWorldCoord;
layout (binding = ID_GBufNormal) uniform sampler2D u_gbufNormal;
layout (binding = ID_GBufSpecular) uniform sampler2D u_gbufSpecular;

layout (binding = ID_GBufDepth_0) uniform sampler2D u_gbufDepth0;
layout (binding = ID_GBufDepth_1toN) uniform sampler2D u_gbufDepth1N;
float fetchGBufDepth(vec2 coord, int mipLevel)
{
    return mipLevel == 0 ? texture(u_gbufDepth0, coord).r :
        textureLod(u_gbufDepth1N, coord, float(mipLevel)).r;
}
ivec2 getGBufDepthSize(int mipLevel)
{
    return mipLevel == 0 ? textureSize(u_gbufDepth0, 0) :
        textureSize(u_gbufDepth1N, mipLevel - 1);
}

layout (binding = ID_BackColor) uniform sampler2D u_backColor;

layout (rgba16f, binding = ID_Target) uniform image2D img_target;

//--------------------------------------------------------------------------------------
//  main function
//--------------------------------------------------------------------------------------

//  dummy functions (required by "SSRayTracing.h" header)
float fetchRSMDepth(vec2 coord, int mipLevel)
{ return 0; }
ivec2 getRSMDepthSize(int mipLevel) 
{ return ivec2(0); }

#include "functions.glsl"
#include "ImageSpaceRT.h"

const float epsilon = 1e-4;

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
int retrieveSample(out vec3 origin, out vec3 direction, out vec2 texCoord)
{
    //  retrieve sampling coordinate (texture space, not normalized yet)
    const vec2 localSamplingCoord = sampleNoise();
    const vec2 samplingCoord = (localSamplingCoord + gl_WorkGroupID.xy) * ivec2(gl_WorkGroupSize.xy * u_params.samplingMapScale);
    const ivec2 gbufDim = textureSize(u_gbufSpecular, 0);
    if (samplingCoord.x >= gbufDim.x || samplingCoord.y >= gbufDim.y)
        return 1; // out-of-bound coordinate

    //  normalize coordinate
    const vec2 normSamplingCoord = samplingCoord / uvec2(gbufDim.x, gbufDim.y);

    //  sample ray payload
    const vec3 worldPos = texture(u_gbufWorldCoord, normSamplingCoord).rgb;
    const vec3 normal = texture(u_gbufNormal, normSamplingCoord).rgb * 2.0f - 1.0f;
    const vec4 specularRoughness = texture(u_gbufSpecular, normSamplingCoord).rgba;

    //  setting roughness cutoff to 30% (perceptualRoughness > 0.3 shall not pass)
    if (specularRoughness.a > 0.3f)
        return 2; // too much roughness for reflective/refractive bouncing

    //  determine the direction of the ray be Fresnel's equation
    vec3 bounceDir;

    const vec3 view = normalize(u_params.camera.position.xyz - worldPos);

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
    texCoord = normSamplingCoord;

    return 0;
}

//  trace the ray through the depth map. 
//  return the hitcolor.
vec4 trace(vec3 origin, vec3 direction)
{
    vec2 hitCoord;
    bool bHit;

    //  tracing in screen space
    float t;
    bHit = traceOnView(
            origin, direction, 
            u_params.camera, true, u_params.tMax,
            t, hitCoord);
    
    //  sampling color from the last hit coordinate
    const vec3 color = bHit ? texture(u_backColor, hitCoord).rgb : vec3(0);

    return vec4(color, 1);
}

void main()
{
    //  get the sampling point first
    vec3 origin;
    vec3 direction;
    vec2 texCoord;
    if (retrieveSample(origin, direction, texCoord) != 0)
        return;

    //  trace through depth map
    vec4 color = trace(origin, direction);
    imageStore(img_target, ivec2(texCoord * textureSize(u_gbufSpecular, 0)), color);
}