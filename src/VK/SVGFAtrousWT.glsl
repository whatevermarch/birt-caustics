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

layout (push_constant) uniform pushConstants
{
    layout (offset = 0) int atrousIterCount;
};

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

layout (binding = ID_Normal) uniform sampler2D u_normal;
layout (binding = ID_DepthMoment) uniform sampler2D u_depthMoment;

layout (rgba16f, binding = ID_InHDR) uniform image2D in_HDR;
layout (rgba16f, binding = ID_ImdHDR) uniform image2D imd_HDR;
vec4 loadHDR(bool toInput, ivec2 texCoord)
{
    if (toInput)
        return imageLoad(imd_HDR, texCoord);
    else
        return imageLoad(in_HDR, texCoord);
}
void storeHDR(bool toInput, ivec2 texCoord, vec4 val)
{
    if (toInput)
        imageStore(in_HDR, texCoord, val);
    else
        imageStore(imd_HDR, texCoord, val);
}
layout (rgba16f, binding = ID_CacheHDR) uniform image2D out_cacheHDR;

//--------------------------------------------------------------------------------------
//  main function
//--------------------------------------------------------------------------------------

#include "functions.glsl"
#include "SVGFEdgeStoppingFunc.h"

// computes a 3x3 gaussian blur of the variance, centered around
// the current pixel
float computeVarianceCenter(bool toInput, ivec2 ipos)
{
    float sum = 0;

    const float kernel[2][2] = {
        { 1.0f / 4.0f, 1.0f / 8.0f  },
        { 1.0f / 8.0f, 1.0f / 16.0f }
    };

    const int radius = 1;
    for (int yy = -radius; yy <= radius; yy++)
    {
        for (int xx = -radius; xx <= radius; xx++)
        {
            const ivec2 p = ipos + ivec2(xx, yy);

            const float k = kernel[abs(xx)][abs(yy)];

            sum += loadHDR(toInput, p).a * k;
        }
    }

    return sum;
}

void main()
{
    //  determine if the output for this a-trous iteration is the input color buffer itself.
    bool toInput = (atrousIterCount & 1) != 0;

    //  retrieve working coordinate
    const ivec2 texSize = textureSize(u_normal, 0);
    const ivec2 texCoord = ivec2(gl_GlobalInvocationID.xy);

    const float epsVariance      = 1e-10;
    const float kernelWeights[3] = { 1.0, 2.0 / 3.0, 1.0 / 6.0 };

    // constant samplers to prevent the compiler from generating code which
    // fetches the sampler descriptor from memory for each texture access
    const vec4  ctrColorVariance = loadHDR(toInput, texCoord);
    const float ctrLuminance = getPerceivedBrightness(ctrColorVariance.xyz);

    // variance for direct and indirect, filtered using 3x3 gaussin blur
    const float ctrVariance = computeVarianceCenter(toInput, texCoord);

    const vec3 ctrNormal = texelFetch(u_normal, texCoord, 0).rgb * 2.0f - 1.0f;

    const vec4 ctrPackedDepthMoment = texelFetch(u_depthMoment, texCoord, 0);
    const float ctrDepth = ctrPackedDepthMoment.x;
    const float fWidthZ = ctrPackedDepthMoment.y;
    const vec2 ctrMoments = ctrPackedDepthMoment.zw;

    if (ctrDepth < -u_params.far || ctrDepth > -u_params.near) // might be envmap
    {
        //  just pass the color (and variance) data
        //out_HDR = ctrColorVariance;
        storeHDR(toInput, texCoord, ctrColorVariance);
        
        //  store color in cache if this is the first iteration
        if(atrousIterCount == 0)
        {
            //out_cacheHDR = sum_colorVariance;
            imageStore(out_cacheHDR, texCoord, ctrColorVariance);
        }

        return;
    }

    const int stepSize = 1 << atrousIterCount; //int(pow(2, atrousIterCount));

    const float phiLuminance = u_params.phiLuminance * sqrt(max(0.0, epsVariance + ctrVariance));
    const float phiDepth = u_params.phiDepth * max(fWidthZ, 1e-8) * stepSize; // due to a-trous holes

    // explicitly store/accumulate center pixel with weight 1 to prevent issues
    // with the edge-stopping functions
    float sum_w = 1.0;
    vec4 sum_colorVariance = ctrColorVariance;

    for (int yy = -2; yy <= 2; yy++)
    {
        for (int xx = -2; xx <= 2; xx++)
        {
            const ivec2 p     = texCoord + ivec2(xx, yy) * stepSize; // due to a-trous holes
            const bool inside = all(greaterThanEqual(p, ivec2(0,0))) && all(lessThan(p, texSize));

            const float kernel = kernelWeights[abs(xx)] * kernelWeights[abs(yy)];

            if (inside && (xx != 0 || yy != 0)) // skip center pixel, it is already accumulated
            {
                const vec4 pColorVariance = loadHDR(toInput, p);
                const float pLuminance = getPerceivedBrightness(pColorVariance.xyz);

                const vec3 pNormal = texelFetch(u_normal, p, 0).rgb * 2.0f - 1.0f;

                const vec4 pPackedDepthMoment = texelFetch(u_depthMoment, p, 0);
                const float pDepth = pPackedDepthMoment.x;
                const vec2 pMoments = pPackedDepthMoment.zw;

                // compute the edge-stopping functions
                const float w = computeEdgeStoppingWeight(
                    ctrDepth, pDepth, phiDepth * length(vec2(xx, yy)),
					ctrNormal, pNormal, u_params.phiNormal, 
                    ctrLuminance, pLuminance, phiLuminance) * kernel;

                // alpha channel contains the variance, therefore the weights need to be squared, see paper for the formula
                sum_w += w;
                sum_colorVariance += vec4(vec3(w), w * w) * pColorVariance;
            }
        }
    }

    // renormalization is different for variance, check paper for the formula
    sum_colorVariance /= vec4(vec3(sum_w), sum_w * sum_w);

    //out_HDR = sum_colorVariance;
    storeHDR(toInput, texCoord, sum_colorVariance);

    //  store color in cache if this is the first iteration
    if(atrousIterCount == 0)
    {
        //out_cacheHDR = sum_colorVariance;
        imageStore(out_cacheHDR, texCoord, sum_colorVariance);
    }
}