#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_ARB_compute_shader  : enable

//--------------------------------------------------------------------------------------
//  CS workgroup definition
//--------------------------------------------------------------------------------------

layout (local_size_x = 32, local_size_y = 32) in;

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

layout (binding = ID_InHDR) uniform sampler2D u_HDR;
layout (binding = ID_Normal) uniform sampler2D u_normal;
layout (binding = ID_DepthMoment) uniform sampler2D u_depthMoment;

layout (binding = ID_History) uniform usampler2D u_historyLength;

layout (rgba16f, binding = ID_OutHDR) uniform image2D out_HDR;
layout (rgba16f, binding = ID_CacheNormal) uniform image2D out_cacheNormal;
layout (rgba16f, binding = ID_CacheDepthMoment) uniform image2D out_cacheDepthMoment;

layout (r8ui, binding = ID_CacheHistory) uniform uimage2D out_cacheHistory;

//--------------------------------------------------------------------------------------
//  main function
//--------------------------------------------------------------------------------------

#include "functions.glsl"
#include "SVGFEdgeStoppingFunc.h"

void main()
{
    //  retrieve working coordinate
    const ivec2 texSize = textureSize(u_HDR, 0);
    const ivec2 texCoord = ivec2(gl_GlobalInvocationID.xy);

    //  copy intermediate data to cache
    const vec4 colorVariance = texelFetch(u_HDR, texCoord, 0);
    const vec3 rawNormal = texelFetch(u_normal, texCoord, 0).rgb;
    const vec4 packedDepthMoment = texelFetch(u_depthMoment, texCoord, 0);
    const uint historyLength = texelFetch(u_historyLength, texCoord, 0).r;

    imageStore(out_cacheNormal, texCoord, vec4(rawNormal, 0));
    imageStore(out_cacheDepthMoment, texCoord, packedDepthMoment);
    imageStore(out_cacheHistory, texCoord, uvec4(historyLength));

    if (historyLength < 4) // not enough temporal history available
    {
		const float ctrLuminance = getPerceivedBrightness(colorVariance.xyz);

        const vec3 ctrNormal = rawNormal * 2.0f - 1.0f;

        const float ctrDepth = packedDepthMoment.x;
        const float fWidthZ = packedDepthMoment.y;
        const vec2 ctrMoments = packedDepthMoment.zw;

        if (ctrDepth < -u_params.far || ctrDepth > -u_params.near) // might be envmap
        {
            //  just pass the color (and variance) data
            //out_HDR = colorVariance;
            imageStore(out_HDR, texCoord, colorVariance);
            return;
        }

        const float phiLuminance = u_params.phiLuminance;
        const float phiDepth = u_params.phiDepth * max(fWidthZ, 1e-8f) * 3.0f;

        // explicitly store/accumulate center pixel with weight 1 to prevent issues
        // with the edge-stopping functions
        float sum_w = 1.0;
        vec3  sum_color = colorVariance.xyz;
        vec2  sum_moments = ctrMoments;

        // compute first and second moment spatially. This code also applies cross-bilateral
        // filtering on the input color samples
        const int radius = 3;
        for (int yy = -radius; yy <= radius; yy++)
        {
            for (int xx = -radius; xx <= radius; xx++)
            {
                const ivec2 p = texCoord + ivec2(xx, yy);
                const bool inside = all(greaterThanEqual(p, ivec2(0,0))) && all(lessThan(p, texSize));

                if (inside && (xx != 0 || yy != 0))  // skip center pixel, it is already accumulated
                {
                    const vec3 pColor = texelFetch(u_HDR, p, 0).rgb;
		            const float pLuminance = getPerceivedBrightness(pColor);

                    const vec3 pNormal = texelFetch(u_normal, p, 0).rgb * 2.0f - 1.0f;

                    const vec4 pPackedDepthMoment = texelFetch(u_depthMoment, p, 0);
                    const float pDepth = pPackedDepthMoment.x;
                    const vec2 pMoments = pPackedDepthMoment.zw;

                    const float w = computeEdgeStoppingWeight(
                        ctrDepth, pDepth, phiDepth * length(vec2(xx, yy)),
						ctrNormal, pNormal, u_params.phiNormal, 
                        ctrLuminance, pLuminance, phiLuminance);

                    sum_w  += w;

                    sum_color   += pColor * w;
					sum_moments += pMoments * w;
                }
            }
        }

        // Clamp sums to >0 to avoid NaNs.
		sum_w = max(sum_w, 1e-6f);

        sum_color /= sum_w;
        sum_moments /= sum_w;

        //  calculate variance
        float variance = max(0, sum_moments.y - sum_moments.x * sum_moments.x);

        // give the variance a boost for the first frames
        variance *= 4.0f / historyLength;

        //out_HDR = vec4(sum_color, variance);
        imageStore(out_HDR, texCoord, vec4(sum_color, variance));
    }
    else
    {
        //  just pass the color (and variance) data
        //out_HDR = colorVariance;
        imageStore(out_HDR, texCoord, colorVariance);
    }
}