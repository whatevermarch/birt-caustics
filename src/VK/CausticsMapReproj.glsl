#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

precision highp float;

layout (location = 0) in vec2 inTexCoord;

struct CMParams
{
	mat4 world;

	mat4 lightView;
	float lightInvTanHalfFovH;
    float lightInvTanHalfFovV;
	float lightFRange;
    float lightNearZ;
};
layout (std140, binding = ID_Params) uniform Params 
{
    CMParams u_params;
};

layout (binding = ID_GBufDepth) uniform sampler2D u_gbufWorldCoord;
layout (binding = ID_RSMDepth) uniform sampler2D u_rsmDepth;
float fetchRSMDepth(vec2 coord)
{
    coord /= 2;
    vec2 bound = 0.5f - (vec2(1) / textureSize(u_rsmDepth, 0)) / 2;
    if(coord.x >= bound.x)
        coord.x = bound.x;
    if(coord.y >= bound.y)
        coord.y = bound.y;

	return texture(u_rsmDepth, coord).r;
}

layout (binding = ID_CausticsMap) uniform sampler2D u_causticsMap;

layout (location = 0) out vec4 out_irradiance;

//  workaround
const float depthBias = 70.0f / 100000.0f;

void discardOccludedFragment(vec4 lightProjCoord)
{
    vec4 shadowTexCoord = lightProjCoord;

    // remember we are splitting the shadow map in 4 quarters 
    shadowTexCoord.x = (1.0 + shadowTexCoord.x) * 0.25;
    shadowTexCoord.y = (1.0 - shadowTexCoord.y) * 0.25;

    if ((shadowTexCoord.y < 0) || (shadowTexCoord.y > .5f) ||
    (shadowTexCoord.x < 0) || (shadowTexCoord.x > .5f) ||
    (shadowTexCoord.z < 0) || (shadowTexCoord.z > 1))
        discard;

    //  we only work on 1st slot, so we don't need to
    //  offsets the center of the shadow map atlas

    shadowTexCoord.z -= depthBias;

    //  we don't need PCF, just sample it directly
    const float sampleDepth = texture(u_rsmDepth, shadowTexCoord.xy).r;
    if (sampleDepth < shadowTexCoord.z) // means the projected position is behind some object
        discard;
}

void main()
{
    const mat4 proj = mat4(
        u_params.lightInvTanHalfFovH, 0, 0, 0, // col1
		0, u_params.lightInvTanHalfFovV, 0, 0, // col2
		0, 0, u_params.lightFRange, -1,		   // ...
		0, 0, u_params.lightNearZ * u_params.lightFRange, 0
	);

    vec4 worldPos = texture(u_gbufWorldCoord, inTexCoord);
    //  super workaround : avoid painting caustics fragment on the irrelevant geometry (non-receiver)
    if (worldPos.a == 0 || worldPos.y > 0.05f)
        discard;

    const vec4 viewPos = u_params.lightView * vec4(worldPos.xyz, 1.0);
    vec4 projPos = proj * viewPos;
    projPos /= projPos.w;

    //  we need to check whether the fragment is occluded or not
    discardOccludedFragment(projPos);

    vec2 cmTexCoord = vec2(0.5f) + vec2(projPos.x, -projPos.y) * 0.5f;

    //  if one fragment passed through this point, 
    //  then it might be illuminated by caustics
    vec4 causticsColor = texture(u_causticsMap, cmTexCoord);
    out_irradiance = vec4(causticsColor.xyz, 1.0f);
}