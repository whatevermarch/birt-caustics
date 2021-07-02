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
const float tMax = 100.0f;

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
    float rayThickness;
    uint padding[2];
};
layout (std140, binding = ID_Params) uniform Params 
{
    CausticsParams u_params;
};

layout (binding = ID_SamplingMap) uniform sampler2D u_samplingMap;
//const uint samplingMapScale = 2;

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
        vec2 invSize = vec2(1) / textureSize(u_rsmDepth0, 0);
        if(coord.x >= 0.5f)
            coord.x = 0.5f - invSize.x;
        if(coord.y >= 0.5f)
            coord.y = 0.5f - invSize.y;
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
    vec2 samplingCoord = (texelFetch(u_samplingMap, ivec2(gl_LocalInvocationID.xy), 0).rg + gl_WorkGroupID.xy) * 
                            gl_WorkGroupSize.xy * u_params.samplingMapScale;
    ivec2 rsmDim = textureSize(u_rsmFlux, 0) / 2; // shadow map in 4 quarters 
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
    vec3 view = normalize(u_params.lights[rsmLightIndex].position.xyz - worldPos);
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
            float iorRatio = 1.0f / IOR; // air -> water
            // float sin_bounceAngle = sin(incidentAngle) * iorRatio;
            // float cos_bounceAngle = sqrt(1.0f - sin_bounceAngle * sin_bounceAngle);

            // vec3 bitangent = normalize(cross(cross(view, normal), normal));
            // bounceDir = sin_bounceAngle * bitangent + cos_bounceAngle * (-normal);
            bounceDir = refract(-view, normal, iorRatio);
        }
    }
    //  for reflected rays
    else
    {
        // vec3 projView_normal = (dot(view, normal) / dot(normal, normal)) * normal;
        // bounceDir = (projView_normal - view) * 2.0f + view;
        bounceDir = reflect(-view, normal);
    }

    origin = worldPos;
    direction = bounceDir;
    //  ToDo : remove this. it is just to visualize first hit
    //direction = -view;
    power = fluxAlpha.xyz;
}

float toViewDepth(float projDepth, float nearPlane, float farPlane)
{ return -nearPlane * farPlane / (projDepth * (nearPlane - farPlane) + farPlane); }

bool traceOnView(vec3 origin, vec3 direction, out float lastT, TransformParams viewParams, bool bCamera)
{
    //  remember the view and clip space is RH (negative Z)
    //  determine projected trace direction
    vec4 viewOri = viewParams.view * vec4(origin, 1.0f);
    vec4 viewDir = viewParams.view * vec4(direction, 0.0f);
    bool bNoViewZMove = abs(viewDir.z) < epsilon;

    //  handle special cases : too narrow direction vector
    //  this is to prevent an overflow of t value also
    float near_linear = viewParams.nearPlane;
    float far_linear = viewParams.farPlane;

    lastT = 0;
    float tMax_actual = tMax;

    if (near_linear > (-viewOri.z) || far_linear < (-viewOri.z))
        return false;
    
    vec2 projVec_xy = vec2(viewParams.invTanHalfFovH, viewParams.invTanHalfFovV);
    vec2 projOri = viewOri.xy * projVec_xy / (-viewOri.z);
    vec2 moveDir;
    {
        vec3 viewDst = viewOri.xyz + viewDir.xyz * tMax;
        if (viewDst.z > -near_linear) // prevent position behind the camera (otherwise, sym. flip image)
        {
            float t_atNear = (-near_linear - viewOri.z) / viewDir.z;
            viewDst = viewOri.xyz + viewDir.xyz * t_atNear;
            tMax_actual = t_atNear;
        }

        vec2 projDst = viewDst.xy * projVec_xy / (-viewDst.z);
        moveDir = projDst - projOri;
    }  

    vec2 startCoord = vec2(0.5f) + vec2(projOri.x, -projOri.y) * 0.5f;
    vec2 unitMoveDir = length(moveDir) < epsilon ? vec2(0) : normalize(vec2(moveDir.x, -moveDir.y));

    //  test
    // float depth_nonlinear = bCamera ? fetchGBufDepth(startCoord, 0) : fetchRSMDepth(startCoord, 0);
    // float depth_linear = toViewDepth(depth_nonlinear, near_linear, far_linear);

    // lastT = (depth_linear - viewOri.z) / viewDir.z;
    // if(lastT >= 0)
    //     return true; // hit
    // else
    //     return false;

    // float variatedEpsilon = 1e-8 + (epsilon - 1e-8) * (-viewOri.z - near_linear) / (far_linear - near_linear);
    // float cosViewDir = dot(normalize(viewOri), viewDir);
    // if (1.0f - cosViewDir < variatedEpsilon)
    // {
    //     float depth_nonlinear = bCamera ? fetchGBufDepth(startCoord, 0) : fetchRSMDepth(startCoord, 0);
    //     float depth_linear = toViewDepth(depth_nonlinear, near_linear, far_linear);

    //     lastT = (depth_linear - viewOri.z) / viewDir.z;
    //     return true; // hit
    // }
    // else if (cosViewDir + 1.0f < variatedEpsilon)
    // {
    //     lastT = (-near_linear - viewOri.z) / viewDir.z;
    //     return false; // miss
    // }

    //  traverse
    bool bFirstTime = true;
    bool bHit = false;
    int traverseLevel = 0;
    int maxTraverseLevel = bCamera ? textureQueryLevels(u_gbufDepth1N) : textureQueryLevels(u_rsmDepth1N);
    vec2 currCoord = startCoord;
    while (traverseLevel >= 0)
    {
        vec2 nextBasis = vec2(1) / (bCamera ? getGBufDepthSize(traverseLevel) : getRSMDepthSize(traverseLevel));
        vec2 nextCoord = currCoord + (bFirstTime ? vec2(0) : unitMoveDir * nextBasis);

        if (nextCoord.x >= 0 && nextCoord.x <= 1 &&
            nextCoord.y >= 0 && nextCoord.y <= 1)
        {
            //  fetch scene's depth value
            float projSamplePos_z = bCamera ? fetchGBufDepth(nextCoord, traverseLevel) : fetchRSMDepth(nextCoord, traverseLevel);
            float viewSamplePos_z = toViewDepth(projSamplePos_z, near_linear, far_linear); // = depth_linear;

            //  calculate predicates if z is equal
            float t_eqZ = bNoViewZMove ? 0 : (viewSamplePos_z - viewOri.z) / viewDir.z;
            vec2 viewRayPos_xy = viewOri.xy + viewDir.xy * t_eqZ;
            vec2 viewSamplePos_xy = vec2(2, -2) * (nextCoord - 0.5f) * (-viewSamplePos_z) / projVec_xy;

            //  calculate predicates if xy are equal
            float t_eqXY;
            float viewRayPos_z; // WARNING!! : not associated with 'viewRayPos_xy'
            if ((!bNoViewZMove) && distance(viewRayPos_xy, viewOri.xy) < epsilon) // this means the ray marches in parallel with view's Z axis
            {
                if (t_eqZ < -epsilon)
                {
                    t_eqXY = (-near_linear - viewOri.z) / viewDir.z;
                    viewRayPos_z = -near_linear;
                }
                else
                {
                    t_eqXY = t_eqZ;
                    viewRayPos_z = viewSamplePos_z;
                }
            }
            else
            {
                if (abs(viewDir.x) > abs(viewDir.y))
                    t_eqXY = (viewSamplePos_xy.x - viewOri.x) / viewDir.x;
                else
                    t_eqXY = (viewSamplePos_xy.y - viewOri.y) / viewDir.y;

                viewRayPos_z = viewOri.z + viewDir.z * t_eqXY;
            }

            //  decide whether the ray hit or not (in XY plane)
            if ((!bNoViewZMove) && distance(viewRayPos_xy, viewSamplePos_xy) < u_params.rayThickness)
            {
                if (traverseLevel == 0)
                {
                    if (t_eqZ < -epsilon)
                        lastT = t_eqXY;
                    else
                    {
                        lastT = t_eqZ;
                        if (viewDir.z < 0 || abs(viewRayPos_z - viewSamplePos_z) <= u_params.rayThickness)
                            bHit = true;
                    }
                }
            }
            else //  decide whether the ray hit or not (along Z axis)
            {
                //  check hit (remember that view depth (linear) is negative !!)
                if (viewRayPos_z >= viewSamplePos_z) // ray is still above the surface
                {
                    if (traverseLevel == 0 && viewRayPos_z - viewSamplePos_z <= u_params.rayThickness)
                    {
                        vec2 neighborCoord = nextCoord + unitMoveDir * nextBasis;

                        //  check gradient
                        if (neighborCoord.x >= 0 && neighborCoord.x <= 1 &&
                            neighborCoord.y >= 0 && neighborCoord.y <= 1)
                        {
                            float neighborProjSamplePos_z = bCamera ? 
                                fetchGBufDepth(neighborCoord, 0) : fetchRSMDepth(neighborCoord, 0);
                            if (neighborProjSamplePos_z >= projSamplePos_z)
                            {
                                if (bFirstTime)
                                    bFirstTime = false;
                                else if (traverseLevel < maxTraverseLevel)
                                    traverseLevel += 1;

                                currCoord = nextCoord;
                                continue;
                            }
                        }
                        lastT = t_eqXY;
                        bHit = true;
                    }
                    else
                    {
                        if (bFirstTime)
                            bFirstTime = false;
                        else if (traverseLevel < maxTraverseLevel)
                            traverseLevel += 1;

                        currCoord = nextCoord;
                        continue;
                    }
                }
                else if (traverseLevel == 0) // ray is hit or occluded
                {
                    if (viewSamplePos_z - viewRayPos_z <= u_params.rayThickness)
                        bHit = true;
                    lastT = t_eqXY;
                }
            }
        }
        
        traverseLevel -= 1;
        maxTraverseLevel = traverseLevel;
    }

    return bHit;
}

//  trace the ray through the depth map and decay the input power in-place. 
//  return the hitpoint (transformed w/ z = depth), along with ray length (t) as w-elemeent.
vec4 trace(vec3 origin, vec3 direction, vec3 power)
{
    vec3 hitPos = vec3(0);
    
    float t = 0;
    vec3 lastPos = origin;
    bool bHit = false;
    
    //  workaround : only trace on RSM first
    bHit = traceOnView(lastPos, direction, t, u_params.lights[rsmLightIndex], false);
    lastPos = lastPos + direction * t;
 
    //  try preemptively project to screen if miss
    // if (!bHit)
    // {
        //  ToDo : continue tracing in screen space
        // ...
    // }

    //  check normal consistency
//    vec2 screenSampleCoord = vec2(0.5f) + vec2(hitPos.x, -hitPos.y);
//    if (dot(-direction, texture(u_gbufNormal, screenSampleCoord).rgb) < 0)
//    {
//        out_hitPos_irradiance[writeIdx] = vec4(0); // if unreachable -> ray length = -1.0
//        return;
//    }

    //  calculate irradiance
    float irradiance_32b;
    if (!bHit)
    {
        irradiance_32b = uintBitsToFloat(0);
    }
    else
    {
        //  save hitpoint
        vec4 viewPos = u_params.camera.view * vec4(lastPos, 1.0f);
        vec2 projPos = viewPos.xy * vec2(u_params.camera.invTanHalfFovH, u_params.camera.invTanHalfFovV) / (-viewPos.z);
        float projConstA = u_params.camera.farPlane / (u_params.camera.nearPlane - u_params.camera.farPlane);
        float projConstB = projConstA * u_params.camera.nearPlane;
        float projDepth = -projConstA + projConstB / (-viewPos.z);

        hitPos = vec3(projPos.x, projPos.y, projDepth);

        //float pixelArea = ...
        //vec3 irradiance /= pixelArea;
        vec3 irradiance = power;

        //  compress irradiance
        irradiance_32b = uintBitsToFloat(packUnorm4x8(vec4(irradiance, 1))); // use uintBitsToFloat() to reinterpret_cast<>()
    }

    return vec4(hitPos, irradiance_32b);
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
    // vec4 viewPos = u_params.camera.view * vec4(origin, 1.0f);
    // vec2 projPos = viewPos.xy * vec2(u_params.camera.invTanHalfFovH, u_params.camera.invTanHalfFovV) / (-viewPos.z);
    // float projConstA = u_params.camera.farPlane / (u_params.camera.nearPlane - u_params.camera.farPlane);
    // float projConstB = projConstA * u_params.camera.nearPlane;
    // float projDepth = -projConstA + projConstB / (-viewPos.z);
    // vec4 hitPos = vec4(projPos.x, projPos.y, projDepth, 1.0f);
    
    out_hitPos_irradiance[writeIdx] = hitPos;
}
