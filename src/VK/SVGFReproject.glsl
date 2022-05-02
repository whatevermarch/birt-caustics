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
//  set 0 : input data
//--------------------------------------------------------------------------------------

struct SVGFParams
{
    float alphaColor;
    float alphaMoments;
    float near;
    float far;

    //  ToDo : actually it is called 'sigma' in the paper. change it!
    float phiDepth;
    float phiNormal;
    float phiLuminance;
    float padding;
};
layout (std140, binding = ID_Params) uniform Params 
{
    SVGFParams u_params;
};

layout (binding = ID_HDR) uniform sampler2D u_HDR;
layout (binding = ID_Normal) uniform sampler2D u_normal;
layout (binding = ID_Depth) uniform sampler2D u_depth;
layout (binding = ID_MotionVec) uniform sampler2D u_motionVec;

layout (binding = ID_CacheHDR) uniform sampler2D u_cacheHDR;
layout (binding = ID_CacheNormal) uniform sampler2D u_cacheNormal;
layout (binding = ID_CacheDepthMoment) uniform sampler2D u_cacheDepthMoment;

layout (binding = ID_CacheHistory) uniform usampler2D u_cacheHistory;

//--------------------------------------------------------------------------------------
//  FS outputs
//--------------------------------------------------------------------------------------

layout (location = 0) out vec4 out_HDR;
layout (location = 1) out vec4 out_DepthMoment;
layout (location = 2) out uint out_History;

//--------------------------------------------------------------------------------------
//  main function
//--------------------------------------------------------------------------------------

#include "functions.glsl"

const float epsilon = 1e-4;

float toViewDepth(float projDepth, float nearPlane, float farPlane)
{ return -nearPlane * farPlane / (projDepth * (nearPlane - farPlane) + farPlane); }

bool isConsistent(float Z, float Zprev, float fwidthZ, vec3 normal, vec3 normalPrev, float fwidthNormal)
{
    // check if deviation of depths is acceptable
    if(abs(Zprev - Z) / (fwidthZ + epsilon) > 2.0)
        return false;

    // check normals for compatibility
    if(distance(normal, normalPrev) / (fwidthNormal + epsilon) > 16.0)
        return false;

    return true;
}

bool loadPrevData(ivec2 texCoord, ivec2 texSize, out vec3 prevItgColor, out vec2 prevMoments, out vec2 packedDepthGradient, out uint historyLength)
{
    //  initialize returned values
    prevItgColor = vec3(0);
    prevMoments = vec2(0);
    historyLength = 0;

    //  retrieve current frame's geometric data
    const vec3 normal = texelFetch(u_normal, texCoord, 0).rgb * 2.0f - 1.0f;
    const float fWidthNormal = length(fwidth(normal));
    const float linearDepth = toViewDepth(texelFetch(u_depth, texCoord, 0).r, u_params.near, u_params.far);
    const float fWidthZ = max(abs(dFdx(linearDepth)), abs(dFdy(linearDepth)));
    packedDepthGradient = vec2(linearDepth, fWidthZ);

    //  determine corresp. data coordinate in the previous frame
    const vec2 motionVec = texelFetch(u_motionVec, texCoord, 0).rg * vec2(0.5, -0.5);
    const vec2 prevUnnormCoord = texCoord - motionVec * texSize - vec2(0.5, 0.5); // since we assume that the integer coord is at the center of the px
    const ivec2 prevTexCoord = ivec2(round(prevUnnormCoord.x), round(prevUnnormCoord.y));

    // check whether reprojected pixel is inside of the screen
    if(any(lessThan(prevTexCoord, ivec2(0))) || any(greaterThan(prevTexCoord, texSize - ivec2(1)))) 
        return false;

    //  examine 2x2 tap
    bool v[4];
    const ivec2 offsets[4] = { ivec2(0, 0), ivec2(1, 0), ivec2(0, 1), ivec2(1, 1) };

    bool valid = false;
    for (int sampleIdx = 0; sampleIdx < 4; sampleIdx++)
    { 
        const ivec2 loc = ivec2(prevUnnormCoord) + offsets[sampleIdx];
        const vec3 prevNormal = texelFetch(u_cacheNormal, loc, 0).rgb * 2.0f - 1.0f;
        const float prevLinearDepth = texelFetch(u_cacheDepthMoment, loc, 0).r;

        v[sampleIdx] = isConsistent(linearDepth, prevLinearDepth, fWidthZ, normal, prevNormal, fWidthNormal);

        valid = valid || v[sampleIdx];
    }

    if (valid) // perform bilinear interpolation
    {
        const float x = fract(prevUnnormCoord.x);
        const float y = fract(prevUnnormCoord.y);
        const float w[4] = { (1 - x) * (1 - y), 
                             x       * (1 - y), 
                             (1 - x) * y,
                             x       * y };

        float sum_w = 0;
        for (int sampleIdx = 0; sampleIdx < 4; sampleIdx++)
        {
            const ivec2 loc = ivec2(prevUnnormCoord) + offsets[sampleIdx];           
            if (v[sampleIdx])
            {
                prevItgColor += w[sampleIdx] * texelFetch(u_cacheHDR, loc, 0).rgb;
                prevMoments += w[sampleIdx] * texelFetch(u_cacheDepthMoment, loc, 0).ba;
                sum_w += w[sampleIdx];
            }
        }

		valid = sum_w >= epsilon;
		prevItgColor = valid ? prevItgColor / sum_w : vec3(0);
		prevMoments = valid ? prevMoments / sum_w : vec2(0);
    }
    else // further extending to 3x3 tap and perform cross-bilateral filtering
    {
        uint v_count = 0;

        const int radius = 1;
        for (int yy = -radius; yy <= radius; yy++)
        {
            for (int xx = -radius; xx <= radius; xx++)
            {
                const ivec2 loc = prevTexCoord + ivec2(xx, yy);
                const vec3 prevNormal = texelFetch(u_cacheNormal, loc, 0).rgb * 2.0f - 1.0f;
                const float prevLinearDepth = texelFetch(u_cacheDepthMoment, loc, 0).r;

                if (isConsistent(linearDepth, prevLinearDepth, fWidthZ, normal, prevNormal, fWidthNormal))
                {
					prevItgColor += texelFetch(u_cacheHDR, loc, 0).rgb;
					prevMoments += texelFetch(u_cacheDepthMoment, loc, 0).ba;
                    v_count++;
                }
            }
        }
        if (v_count > 0)
        {
            valid = true;
            prevItgColor /= v_count;
            prevMoments /= v_count;
        }
    }

    //  retrieve history length if there is no disocclusion
    if (valid)
        historyLength = texelFetch(u_cacheHistory, prevTexCoord, 0).r;

    return valid;
}

void main()
{
    //  retrieve current pixel color
    const ivec2 texSize = textureSize(u_HDR, 0);
    // const ivec2 texCoord = gl_WorkGroupID.xy * gl_WorkGroupSize.xy + gl_LocalInvocationID.xy;
    const ivec2 texCoord = ivec2(floor(vec2(UV.x, /*1.0 - */UV.y) * texSize));
    const vec3 color = clamp(texelFetch(u_HDR, texCoord, 0).rgb, 0.0f, 1.0f);

    //  rretrieve previous frame's data
    vec3 prevItgColor;
    vec2 prevMoments;
    vec2 packedDepthGradient;
    uint historyLength;
    bool success = loadPrevData(texCoord, texSize, prevItgColor, prevMoments, packedDepthGradient, historyLength);

    //  increment history length if no disocclusion happens (if not, success = true)
    historyLength = min(32, success ? historyLength + 1 : 1);

    //  This adjusts the alpha for the case where insufficient history is available.
    //  It boosts the temporal accumulation to give the samples equal weights in
    //  the beginning.
    const float alphaColor = success ? max(u_params.alphaColor, 1.0 / historyLength) : 1.0;
    const float alphaMoments = success ? max(u_params.alphaMoments, 1.0 / historyLength) : 1.0;

    //  perform temporal accumulation on color
    const vec3 itgColor = mix(prevItgColor, color, alphaColor);

    //  perform temporal accumulation on luminance (both L and L^2)
    const float luminance = getPerceivedBrightness(color);
    const vec2 moments = mix(prevMoments, vec2(/* L */luminance, /* L^2 */luminance * luminance), alphaMoments);

    //  calculate variance
    const float variance = max(0, moments.y - moments.x * moments.x);

    //  save integrated values and variance
    out_HDR = vec4(itgColor, variance);
    out_DepthMoment = vec4(packedDepthGradient, moments);
    out_History = historyLength;
}