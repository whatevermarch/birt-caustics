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

struct TransformParams
{
    mat4 view;
    vec4 position;
    float invTanHalfFovH;
    float invTanHalfFovV;
    float nearPlane;
    float farPlane;
};
struct CausticsParams
{
    TransformParams camera;
    TransformParams lights[4]; // we have 4 quarters of RSM

    float samplingMapScale;
    float rayThickness_xy;
    float rayThickness_z;
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
    // return mipLevel == 0 ? texture(u_rsmDepth0, coord).r :
    //     textureLod(u_rsmDepth1N, coord, float(mipLevel - 1)).r;
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
        textureLod(u_gbufDepth1N, coord, float(mipLevel)).r;
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

const float IOR = 1.33;
const float epsilon = 1e-4;

// random function
// ref : https://thebookofshaders.com/10/
float randFromCoord (vec2 st) {
    return fract(sin(dot(st, vec2(12.9898,78.233))) * 43758.5453123);
}

// Exact, unpolarized Fresnel equation
float fresnelUnpolarized(
    float cosI, // cosIncident
    float cosT, // cosTransmitted
    float n1, // refractiveIndexIncident,
    float n2 // refractiveIndexTransmitted
)
{
    float rs = (n1 * cosI - n2 * cosT) / (n1 * cosI + n2 * cosT);
    float rp = (n1 * cosT - n2 * cosI) / (n1 * cosT + n2 * cosI);
    return 0.5f * (rs * rs + rp * rp);
}

//  retrieve the sample point from RSM and construct ray payload
int retrieveSample(out vec3 origin, out vec3 direction, out vec3 power)
{
    //  retrieve sampling coordinate (texture space, not normalized yet)
    const vec2 localSamplingCoord = texelFetch(u_samplingMap, ivec2(gl_LocalInvocationID.xy), 0).rg;
    vec2 samplingCoord = (localSamplingCoord + gl_WorkGroupID.xy) * ivec2(gl_WorkGroupSize.xy * u_params.samplingMapScale);
    const ivec2 rsmDim = textureSize(u_rsmFlux, 0) / 2; // shadow map in 4 quarters 
    if (samplingCoord.x >= rsmDim.x || samplingCoord.y >= rsmDim.y)
        return 1; // out-of-bound coordinate

    //  normalize coordinate
    samplingCoord /= uvec2(rsmDim.x, rsmDim.y);

    //  sample ray payload
    // remember we are splitting the shadow map in 4 quarters 
    samplingCoord *= 0.5f;

    // offsets of the center of the shadow map atlas
    const float offsetsX[4] = { 0.0, 0.5, 0.0, 0.5 };
    const float offsetsY[4] = { 0.0, 0.0, 0.5, 0.5 };
    samplingCoord += vec2(offsetsX[rsmLightIndex], offsetsY[rsmLightIndex]);

    const vec3 worldPos = texture(u_rsmWorldCoord, samplingCoord).rgb;
    const vec3 normal = texture(u_rsmNormal, samplingCoord).rgb * 2.0f - 1.0f;
    const vec4 specularRoughness = texture(u_rsmSpecular, samplingCoord).rgba;
    const vec4 fluxAlpha = texture(u_rsmFlux, samplingCoord).rgba;

    //  setting roughness cutoff to 30% (perceptualRoughness > 0.3 shall not pass)
    if (specularRoughness.a > 0.3f)
        return 2; // too much roughness for reflective/refractive bouncing

    //  filter negligible photon with too low flux power
    if (getPerceivedBrightness(fluxAlpha.xyz) < 0.04f) // ref. f0 value from 'PixelParams.glsl'
        return 3;

    //  determine the direction of the ray be Fresnel's equation
    vec3 bounceDir;

    const vec3 view = normalize(u_params.lights[rsmLightIndex].position.xyz - worldPos);

    const float iorRatio = 1.0f / IOR; // air -> water
    const vec3 refracted = refract(-view, normal, iorRatio);

    // In the case of total internal reflection the refract() function
    // returns (0,0,0) and Fresnel has to be 1.
    float fresnel = 1.0f;
    if (any(greaterThan(refracted, vec3(0)))) 
    {
        // Compute Fresnel using the Fresnel equations (not the Schlick
        // approximation!) This is more precise, especially if the
        // difference between the two indices of
        // refraction is very small, and avoids a special case
        const float cosIncident = dot(normal, view);
        const float cosTransmitted = -dot(refracted, normal);

        fresnel = fresnelUnpolarized(cosIncident, cosTransmitted, 1.0f, IOR);
    }

    // random and see if we go for reflection or refraction
    if (randFromCoord(localSamplingCoord) < fresnel) // reflection
        bounceDir = reflect(-view, normal);
    else // refraction
        bounceDir = refracted;

    origin = worldPos;
    direction = bounceDir;
    power = fluxAlpha.xyz;

    return 0;
}

float toViewDepth(float projDepth, float nearPlane, float farPlane)
{ return -nearPlane * farPlane / (projDepth * (nearPlane - farPlane) + farPlane); }

bool traceOnView(vec3 origin, vec3 direction, TransformParams viewParams, bool bCamera, out float lastT, out vec2 lastCoord)
{
    lastT = 0;
    lastCoord = vec2(-1);

    //  remember the view and clip space is RH (negative Z)
    //  determine projected trace direction
    const vec4 viewOri = viewParams.view * vec4(origin, 1.0f);
    const vec4 viewDir = viewParams.view * vec4(direction, 0.0f);

    //  handle special cases : too narrow direction vector
    //  this is to prevent an overflow of t value also
    const float near_linear = viewParams.nearPlane;
    const float far_linear = viewParams.farPlane;
    
    float tMax_actual = u_params.tMax;

    if (near_linear > (-viewOri.z) || far_linear < (-viewOri.z))
        return false;
    
    const vec2 projVec_xy = vec2(viewParams.invTanHalfFovH, viewParams.invTanHalfFovV);
    const vec2 projOri = viewOri.xy * projVec_xy / (-viewOri.z);
    vec2 moveDir;
    {
        vec3 viewDst = viewOri.xyz + viewDir.xyz * tMax_actual;

        //  pre-bound for near-far plane
        if (viewDst.z > -near_linear)
        {
            float t_atNear = (-near_linear - viewOri.z) / viewDir.z;
            viewDst = viewOri.xyz + viewDir.xyz * t_atNear;
            tMax_actual = t_atNear;
        }
        else if (viewDst.z < -far_linear)
        {
            float t_atFar = (-far_linear - viewOri.z) / viewDir.z;
            viewDst = viewOri.xyz + viewDir.xyz * t_atFar;
            tMax_actual = t_atFar;
        }

        vec2 projDst = viewDst.xy * projVec_xy / (-viewDst.z);
        moveDir = projDst - projOri;
    }

    //  initialize ray info
    lastCoord = vec2(0.5f) + vec2(projOri.x, -projOri.y) * 0.5f;

    const float lastProjSampleDepth = bCamera ? fetchGBufDepth(lastCoord, 0) : fetchRSMDepth(lastCoord, 0);
    const float lastViewSampleDepth = toViewDepth(lastProjSampleDepth, near_linear, far_linear);

    if (abs(viewOri.z - lastViewSampleDepth) <= u_params.rayThickness_z)
        return true;
    else if (viewOri.z < lastViewSampleDepth)
        return false;
    
    //  calculate unit move direction in image space
    if (length(moveDir / 2) < epsilon)
    {
        if (viewDir.z < 0)
        {
            lastT = (lastViewSampleDepth - viewOri.z) / viewDir.z;
            return (lastProjSampleDepth < 1.0f) ? true : false; // hit
        }
        else
        {
            lastT = (-near_linear - viewOri.z) / viewDir.z;
            return false; // miss
        }
    }
    const vec2 unitMoveDir = normalize(vec2(moveDir.x, -moveDir.y));

    //  traverse for the first occlusion first
    bool bHit = false;
    int traverseLevel = 0;
    int maxTraverseLevel = 0; //bCamera ? textureQueryLevels(u_gbufDepth1N) : textureQueryLevels(u_rsmDepth1N);
    while (traverseLevel >= 0)
    {
        const vec2 nextBasis = vec2(1) / (bCamera ? getGBufDepthSize(traverseLevel) : getRSMDepthSize(traverseLevel));
        const vec2 nextCoord = lastCoord + unitMoveDir * nextBasis;

        if (nextCoord.x >= 0 && nextCoord.x <= 1 &&
            nextCoord.y >= 0 && nextCoord.y <= 1)
        {
            //  fetch scene's depth value
            const float nextProjSamplePos_z = bCamera ? fetchGBufDepth(nextCoord, traverseLevel) : fetchRSMDepth(nextCoord, traverseLevel); // = depth_nonlinear;
            const float nextViewSamplePos_z = toViewDepth(nextProjSamplePos_z, near_linear, far_linear); // = depth_linear;

            const vec2 nextProjSamplePos_xy = vec2(2, -2) * (nextCoord - 0.5f);

            //  calculate t if xy-coord (view space) is equal
            const float t_eqXY = abs(viewDir.x) > abs(viewDir.y) ?
                (-nextProjSamplePos_xy.x * viewOri.z - projVec_xy.x * viewOri.x) / 
                    (projVec_xy.x * viewDir.x + nextProjSamplePos_xy.x * viewDir.z) : 
                (-nextProjSamplePos_xy.y * viewOri.z - projVec_xy.y * viewOri.y) / 
                    (projVec_xy.y * viewDir.y + nextProjSamplePos_xy.y * viewDir.z);

            if (t_eqXY <= tMax_actual)
            {
                const float nextViewRayEndPos_z = viewOri.z + viewDir.z * t_eqXY;

                //  check hit (remember that view depth (linear) is negative !!)
                if (nextViewRayEndPos_z > nextViewSamplePos_z + epsilon) // ray is still above the surface
                {
                    if (traverseLevel < maxTraverseLevel)
                        traverseLevel += 1;

                    lastT = t_eqXY;
                    lastCoord = nextCoord;
                    continue;
                }
                else if (traverseLevel == 0) // ray is hit or occluded
                {
                    if (nextViewRayEndPos_z >= nextViewSamplePos_z)
                    {
                        lastT = t_eqXY;
                        lastCoord = nextCoord;
                        bHit = true;
                    }
                    else
                    {
                        //  perform linear backtracking to fing an exact hit point
                        const float prevProjSamplePos_z = bCamera ? fetchGBufDepth(lastCoord, traverseLevel) : fetchRSMDepth(lastCoord, traverseLevel); // = depth_nonlinear;
                        const float prevViewSamplePos_z = toViewDepth(prevProjSamplePos_z, near_linear, far_linear); // = depth_linear;

                        const float prevViewRayEndPos_z = viewOri.z + viewDir.z * lastT;

                        const float dzdtSample = nextViewSamplePos_z - prevViewSamplePos_z;
                        const float dzdtRay = nextViewRayEndPos_z - prevViewRayEndPos_z;
                    
                        if (abs(dzdtSample - dzdtRay) >= epsilon)
                        {
                            const float a_intersect = (prevViewSamplePos_z - prevViewRayEndPos_z) / (dzdtRay - dzdtSample);
                            if (0 <= a_intersect && a_intersect <= 1)
                            {
                                const float t_intersect = mix(lastT, t_eqXY, a_intersect);
                                lastT = t_intersect;
                                const vec2 coord_intersect = mix(lastCoord, nextCoord, a_intersect);
                                lastCoord = coord_intersect;

                                bHit = true;
                            }
                        }
                    }
                }
            }
            else if(traverseLevel == 0) // in case of exceeding t, use tMax as the last stop
            {
                lastT = tMax_actual;

                const vec3 lastViewRayEndPos = viewOri.xyz + viewDir.xyz * tMax_actual;
                const vec2 lastProjRayEndPos = lastViewRayEndPos.xy * projVec_xy / (-lastViewRayEndPos.z);
                lastCoord = vec2(0.5f) + vec2(lastProjRayEndPos.x, -lastProjRayEndPos.y) * 0.5f;
            }
        }
        else if (traverseLevel == 0)
        {
            vec2 lastT_atEdge = vec2(tMax_actual);
            if (nextCoord.x < 0) // exceed -1
                lastT_atEdge.x = (viewOri.z - projVec_xy.x * viewOri.x) / (viewDir.x * projVec_xy.x - viewDir.z);
            else if (nextCoord.x > 1) // exceed 1
                lastT_atEdge.x = (-viewOri.z - projVec_xy.x * viewOri.x) / (viewDir.x * projVec_xy.x + viewDir.z);
            if (nextCoord.y < 0) // exceed 1
                lastT_atEdge.y = (-viewOri.z - projVec_xy.y * viewOri.y) / (viewDir.y * projVec_xy.y + viewDir.z);
            else if (nextCoord.y > 1) // exceed -1
                lastT_atEdge.y = (viewOri.z - projVec_xy.y * viewOri.y) / (viewDir.y * projVec_xy.y - viewDir.z);

            if (lastT_atEdge.x < lastT_atEdge.y) // end at horizontal edge
            {
                lastT = lastT_atEdge.x;

                float bound = (unitMoveDir.x < 0) ? 0.0f : 1.0f;
                lastCoord.y = lastCoord.y + unitMoveDir.y * (bound - lastCoord.x) / unitMoveDir.x;
                lastCoord.x = bound;
            }
            else // end at vertical edge
            {
                lastT = lastT_atEdge.y;

                float bound = (unitMoveDir.y < 0) ? 0.0f : 1.0f;
                lastCoord.x = lastCoord.x + unitMoveDir.x * (bound - lastCoord.y) / unitMoveDir.y;
                lastCoord.y = bound;
            }

            bHit = false;
        }
        
        traverseLevel -= 1;
        maxTraverseLevel = traverseLevel;
    }

    return bHit;
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
    traceOnView(lastPos, direction, u_params.lights[rsmLightIndex], false, t, lastCoord);
    lastPos = lastPos + direction * t;
    lastT += t;

    // continue tracing in screen space
    bHit = traceOnView(lastPos, direction, u_params.camera, true, t, lastCoord);
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
        if (dot(-direction, visibleNormal) >= -epsilon &&
            abs(viewPos.z - visibleDepth) <= u_params.rayThickness_z) 
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
            const vec3 irradiance = power * invPixelArea;

            //  compress irradiance
            irradiance_32b = uintBitsToFloat(float3ToRGBE(irradiance));
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
