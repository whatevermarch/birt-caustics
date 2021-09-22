// Portions Copyright 2019 Advanced Micro Devices, Inc.All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// This file is derived from 'GLTFPBRLighting.h' and 'shadowFiltering.h'
// to reduce type dependecies between the files in Cauldron framework. 
// All credits should go to the original authors.

//
// This fragment shader defines a reference implementation for Physically Based Shading of
// a microfacet surface material defined by a glTF model.
//
// References:
// [1] Real Shading in Unreal Engine 4
//     http://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf
// [2] Physically Based Shading at Disney
//     http://blog.selfshadow.com/publications/s2012-shading-course/burley/s2012_pbs_disney_brdf_notes_v3.pdf
// [3] README.md - Environment Maps
//     https://github.com/KhronosGroup/glTF-WebGL-PBR/#environment-maps
// [4] "An Inexpensive BRDF Model for Physically based Rendering" by Christophe Schlick
//     https://www.cs.virginia.edu/~jdl/bib/appearance/analytic%20models/schlick94b.pdf

#define USE_PUNCTUAL

struct MaterialInfo
{
    float perceptualRoughness;    // roughness value, as authored by the model creator (input to shader)
    vec3 reflectance0;            // full reflectance color (normal incidence angle)

    float alphaRoughness;         // roughness mapped to a more linear change in the roughness (proposed by [2])
    vec3 diffuseColor;            // color contribution from diffuse lighting

    vec3 reflectance90;           // reflectance color at grazing angle
    vec3 specularColor;           // color contribution from specular lighting
};

// Lambert lighting
// see https://seblagarde.wordpress.com/2012/01/08/pi-or-not-to-pi-in-game-lighting-equation/
vec3 diffuse(MaterialInfo materialInfo)
{
    return materialInfo.diffuseColor / M_PI;
}

// The following equation models the Fresnel reflectance term of the spec equation (aka F())
// Implementation of fresnel from [4], Equation 15
vec3 specularReflection(MaterialInfo materialInfo, AngularInfo angularInfo)
{
    return materialInfo.reflectance0 + (materialInfo.reflectance90 - materialInfo.reflectance0) * pow(clamp(1.0 - angularInfo.VdotH, 0.0, 1.0), 5.0);
}

// Smith Joint GGX
// Note: Vis = G / (4 * NdotL * NdotV)
// see Eric Heitz. 2014. Understanding the Masking-Shadowing Function in Microfacet-Based BRDFs. Journal of Computer Graphics Techniques, 3
// see Real-Time Rendering. Page 331 to 336.
// see https://google.github.io/filament/Filament.md.html#materialsystem/specularbrdf/geometricshadowing(specularg)
float visibilityOcclusion(MaterialInfo materialInfo, AngularInfo angularInfo)
{
    float NdotL = angularInfo.NdotL;
    float NdotV = angularInfo.NdotV;
    float alphaRoughnessSq = materialInfo.alphaRoughness * materialInfo.alphaRoughness;

    float GGXV = NdotL * sqrt(NdotV * NdotV * (1.0 - alphaRoughnessSq) + alphaRoughnessSq);
    float GGXL = NdotV * sqrt(NdotL * NdotL * (1.0 - alphaRoughnessSq) + alphaRoughnessSq);

    float GGX = GGXV + GGXL;
    if (GGX > 0.0)
    {
        return 0.5 / GGX;
    }
    return 0.0;
}

// The following equation(s) model the distribution of microfacet normals across the area being drawn (aka D())
// Implementation from "Average Irregularity Representation of a Roughened Surface for Ray Reflection" by T. S. Trowbridge, and K. P. Reitz
// Follows the distribution function recommended in the SIGGRAPH 2013 course notes from EPIC Games [1], Equation 3.
float microfacetDistribution(MaterialInfo materialInfo, AngularInfo angularInfo)
{
    float alphaRoughnessSq = materialInfo.alphaRoughness * materialInfo.alphaRoughness;
    float f = (angularInfo.NdotH * alphaRoughnessSq - angularInfo.NdotH) * angularInfo.NdotH + 1.0;
    return alphaRoughnessSq / (M_PI * f * f + 0.000001f);
}

vec3 getPointShade(vec3 pointToLight, MaterialInfo materialInfo, vec3 normal, vec3 view)
{
    AngularInfo angularInfo = getAngularInfo(pointToLight, normal, view);

    if (angularInfo.NdotL > 0.0 || angularInfo.NdotV > 0.0)
    {
        // Calculate the shading terms for the microfacet specular shading model
        vec3 F = specularReflection(materialInfo, angularInfo);
        float Vis = visibilityOcclusion(materialInfo, angularInfo);
        float D = microfacetDistribution(materialInfo, angularInfo);

        // Calculation of analytical lighting contribution
        vec3 diffuseContrib = (1.0 - F) * diffuse(materialInfo);
        vec3 specContrib = F * Vis * D;

        // Obtain final intensity as reflectance (BRDF) scaled by the energy of the light (cosine law)
        return angularInfo.NdotL * (diffuseContrib + specContrib);
    }

    return vec3(0.0, 0.0, 0.0);
}

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

// shadowmap filtering
float FilterShadow(vec3 uv)
{
    float shadow = 0.0;
#ifdef ID_shadowMap
    ivec2 texDim = textureSize(u_shadowMap, 0);
    float scale = 1.0;
    float dx = scale * 1.0 / float(texDim.x);
    float dy = scale * 1.0 / float(texDim.y);

    int kernelLevel = 2;
    int kernelWidth = 2 * kernelLevel + 1;
    for (int i = -kernelLevel; i <= kernelLevel; i++)
    {
        for (int j = -kernelLevel; j <= kernelLevel; j++)
        {
            float visibility = texture(u_shadowMap, uv + vec3(dx * i, dy * j, 0)).r;
            uint isOpaque = texture(u_opacityMap, uv.xy + vec2(dx * i, dy * j)).r;

            if (visibility == 0 && isOpaque == 0)
            {
                float decay = 0.99f; // ToDo : decay = alpha (fetched from rsm-Flux)
                visibility = texture(u_shadowMapBase, uv + vec3(dx * i, dy * j, 0)).r * (1.0f - decay);
            }

            shadow += visibility;
        }
    }

    shadow /= (kernelWidth * kernelWidth);
#endif
    return shadow;
}

//
// Project world space point onto shadowmap
//
float DoSpotShadow(vec3 vPosition, Light light)
{
#ifdef ID_shadowMap
    if (light.shadowMapIndex < 0 || light.shadowMapIndex >= 4)
        return 1.0f;

    if (light.type != LightType_Spot && light.type != LightType_Directional)
        return 1.0; // no other light types cast shadows for now

    vec4 shadowTexCoord = light.mLightViewProj * vec4(vPosition, 1.0);
    shadowTexCoord.xyz = shadowTexCoord.xyz / shadowTexCoord.w;

    // remember we are splitting the shadow map in 4 quarters 
    shadowTexCoord.x = (1.0f + shadowTexCoord.x) * 0.25f;
    shadowTexCoord.y = (1.0f - shadowTexCoord.y) * 0.25f;

    if (light.type == LightType_Spot)
    {
        if ((shadowTexCoord.y < 0) || (shadowTexCoord.y > .5)) return 0;
        if ((shadowTexCoord.x < 0) || (shadowTexCoord.x > .5)) return 0;
        if (shadowTexCoord.z < 0.0f) return 0.0f;
        if (shadowTexCoord.z > 1.0f) return 1.0f;
    }
    else if (light.type == LightType_Directional)
    {
        // This is the sun, so outside of the volume we do have light
        if ((shadowTexCoord.y < 0) || (shadowTexCoord.y > .5)) return 1.0f;
        if ((shadowTexCoord.x < 0) || (shadowTexCoord.x > .5)) return 1.0f;
        if (shadowTexCoord.z < 0.0f) return 1.0f;
        if (shadowTexCoord.z > 1.0f) return 1.0f;
    }

    // offsets of the center of the shadow map atlas
    float offsetsX[4] = { 0.0, 1.0, 0.0, 1.0 };
    float offsetsY[4] = { 0.0, 0.0, 1.0, 1.0 };
    shadowTexCoord.x += offsetsX[light.shadowMapIndex] * .5f;
    shadowTexCoord.y += offsetsY[light.shadowMapIndex] * .5f;

    shadowTexCoord.z -= light.depthBias;

    return FilterShadow(shadowTexCoord.xyz);
#else
    return 1.0f;
#endif
}

//  this function does closely to what GLTFPBRLighting.h::doPbrLighting(VS2PS Input, ...) do
vec3 doPbrLighting(vec3 worldPos, vec3 normal, vec3 diffuseColor, vec3 specularColor, float perceptualRoughness)
{
    vec3 intensity = vec3(0);

#ifdef MATERIAL_UNLIT
    intensity = diffuseColor;
    return intensity;
#endif    

    // Roughness is authored as perceptual roughness; as is convention,
    // convert to material roughness by squaring the perceptual roughness [2].
    float alphaRoughness = perceptualRoughness * perceptualRoughness;

    // Compute reflectance.
    float reflectance = max(max(specularColor.r, specularColor.g), specularColor.b);

    vec3 specularEnvironmentR0 = specularColor.rgb;
    // Anything less than 2% is physically impossible and is instead considered to be shadowing. Compare to "Real-Time-Rendering" 4th editon on page 325.
    vec3 specularEnvironmentR90 = vec3(clamp(reflectance * 50.0, 0.0, 1.0));

    MaterialInfo materialInfo = MaterialInfo(
        perceptualRoughness,
        specularEnvironmentR0,
        alphaRoughness,
        diffuseColor,
        specularEnvironmentR90,
        specularColor
    );

    vec3 view = normalize(myPerFrame.u_CameraPos.xyz - worldPos);

#ifdef USE_PUNCTUAL
    for (int i = 0; i < myPerFrame.u_lightCount; ++i)
    {
        Light light = myPerFrame.u_lights[i];

        if (light.type == LightType_Directional)
        {
            vec3 pointToLight = light.direction;
            vec3 shade = getPointShade(pointToLight, materialInfo, normal, view);
            float shadow = DoSpotShadow(worldPos, light);
            intensity += getDirectionalLightFlux(light) * shade * shadow;
        }
        else if (light.type == LightType_Point)
        {
            vec3 pointToLight = light.position - worldPos;
            vec3 shade = getPointShade(pointToLight, materialInfo, normal, view);
            intensity += getPointLightFlux(light, pointToLight) * shade;
        }
        else if (light.type == LightType_Spot)
        {
            vec3 pointToLight = light.position - worldPos;
            vec3 shade = getPointShade(pointToLight, materialInfo, normal, view);
            float shadow = DoSpotShadow(worldPos, light);
            intensity += getSpotLightFlux(light, pointToLight) * shade * shadow;
        }
    }
#endif

    //  workaround : add more ambient light to Cornell Box
    intensity += 0.07f * diffuseColor;

    return intensity;
}
