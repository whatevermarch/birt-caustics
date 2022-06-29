#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

precision highp float;

layout (location = ID_POSITION) in vec3 a_Position;
layout (location = ID_NORMAL) in vec3 a_Normal;

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

//layout (binding = ID_WorldCoord) uniform sampler2D u_worldCoord; // can't use, because it's not receiver.
layout (binding = ID_Depth) uniform sampler2D u_depth;
float fetchDepth(vec2 coord)
{
    coord /= 2;
    vec2 bound = 0.5f - (vec2(1) / textureSize(u_depth, 0)) / 2;
    if(coord.x >= bound.x)
        coord.x = bound.x;
    if(coord.y >= bound.y)
        coord.y = bound.y;

	return texture(u_depth, coord).r;
}

layout (location = 0) out vec4 out_pointColor;
layout (location = 1) out float out_valid;

#include "TransformParams.glsl"

const float EPS = 1e-4;
const float IOR = 2.4; // water
const float depthBias = 70.0f / 100000.0f;

float discardOccludedVertex(vec4 lightProjCoord)
{
    vec4 shadowTexCoord = lightProjCoord;

    // remember we are splitting the shadow map in 4 quarters 
    shadowTexCoord.x = (1.0 + shadowTexCoord.x) * 0.25;
    shadowTexCoord.y = (1.0 - shadowTexCoord.y) * 0.25;

    if ((shadowTexCoord.y < 0) || (shadowTexCoord.y > .5f) ||
    (shadowTexCoord.x < 0) || (shadowTexCoord.x > .5f) ||
    (shadowTexCoord.z < 0) || (shadowTexCoord.z > 1))
        return 0;

    //  we only work on 1st slot, so we don't need to
    //  offsets the center of the shadow map atlas

    shadowTexCoord.z -= depthBias;

    //  we don't need PCF, just sample it directly
    const float sampleDepth = texture(u_depth, shadowTexCoord.xy).r;
	//const float sampleDepth = texture(u_depth, vec2(shadowTexCoord.x, 1.0f - shadowTexCoord.y)).r;
    if (sampleDepth < shadowTexCoord.z) // means the projected position is behind some object
        return 0;
	else
		return 1;
}

const float innerConeAngle = 0.333794f;
const float outerConeAngle = 0.392699f;

// https://github.com/KhronosGroup/glTF/blob/master/extensions/2.0/Khronos/KHR_lights_punctual/README.md#inner-and-outer-cone-angles
float getSpotAttenuation(vec3 viewPos)
{
    //  in view space
    const vec3 spotDirection = vec3(0, 0, -1);
	const float innerConeCos = cos(innerConeAngle);
	const float outerConeCos = cos(outerConeAngle);

    float actualCos = dot(spotDirection, normalize(viewPos));
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

void main()
{
	const mat4 worldView = u_params.lightView * u_params.world;
	const mat4 proj = mat4(
		u_params.lightInvTanHalfFovH, 0, 0, 0, // col1
		0, u_params.lightInvTanHalfFovV, 0, 0, // col2
		0, 0, u_params.lightFRange, -1,		   // ...
		0, 0, u_params.lightNearZ * u_params.lightFRange, 0
	);

	const vec3 v = (worldView * vec4(a_Position, 1)).xyz;
	const vec3 n = normalize((worldView * vec4(a_Normal, 0)).xyz);

	//	vector from light source to v
	//	since v is in view space(light src), the vector to v is exactly normalized v.
	const vec3 i = normalize(v);

	const float iorRatio = 1.0f / IOR; // air -> water
	const vec3 r = refract(i, n, iorRatio);

	uint numIters = 0;
	float d = 1;
	vec4 projPos, projPos_noDiv;
	float depthOnReceiver;
	vec4 projOrigin = proj * vec4(v, 1);
	projOrigin /= projOrigin.w;
	const vec2 originCoord = vec2(0.5f) + vec2(projOrigin.x, -projOrigin.y) * 0.5f;
	
	//	workaround!!
	//	check if the origin is within the light cone and is not occluded
	out_valid = discardOccludedVertex(projOrigin) * getSpotAttenuation(v);
	if (out_valid > 0)
	{
		do
		{
			if(numIters >= 10)
			{
				out_valid = 0;
				break;
			}

			const vec3 p1 = v + d * r;

			projPos = proj * vec4(p1, 1);
			projPos_noDiv = projPos;

			projPos /= projPos.w; // prespective division

			//	lookup position texture
			vec2 targetCoord = vec2(0.5f) + vec2(projPos.x, -projPos.y) * 0.5f;
			const vec2 direction = targetCoord - originCoord;

			//	handle out-of-range cases
			if ((targetCoord.y < 0) || (targetCoord.y > 1) ||
				(targetCoord.x < 0) || (targetCoord.x > 1))
			{
				if (numIters == 0)
				{
					if (targetCoord.x < 0) // exceed -1
						targetCoord = vec2(0, (-originCoord.x / direction.x) * direction.y + originCoord.y);
					else if (targetCoord.x > 1) // exceed 1
						targetCoord = vec2(1, ((1.0f - originCoord.x) / direction.x) * direction.y + originCoord.y);
					if (targetCoord.y < 0) // exceed -1
						targetCoord = vec2((-originCoord.y / direction.y) * direction.x + originCoord.x, 0);
					else if (targetCoord.y > 1) // exceed 1
						targetCoord = vec2(((1.0f - originCoord.y) / direction.y) * direction.x + originCoord.x, 1);
				}
				else
				{
					out_valid = 0;
					break;
				}
			}

			depthOnReceiver = fetchDepth(targetCoord);
			const vec3 posOnReceiver = toViewCoord(
				targetCoord, depthOnReceiver,
				u_params.lightInvTanHalfFovH, u_params.lightInvTanHalfFovV,
				u_params.lightFRange, u_params.lightNearZ
			);

			d = length(posOnReceiver - v);
			numIters += 1;
		} 
		while(abs(projPos.z - depthOnReceiver) > EPS);
	}

	if(out_valid > 0)
	{
		gl_Position = projPos_noDiv;
		gl_PointSize = 52.0f; // manually adjusted
	}
	else
	{
		gl_Position = vec4(0, 0, 0, 1);
		gl_PointSize = 1.0f; // manually adjusted
	}

	out_pointColor = vec4(1);
}