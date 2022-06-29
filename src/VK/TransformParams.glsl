struct TransformParams
{
    mat4 view;
    vec4 position;
    float invTanHalfFovH;
    float invTanHalfFovV;
    float nearPlane;
    float farPlane;
};

mat4 constructProjMatrix(float invTanHalfFovH, float invTanHalfFovV, float nearZ, float farZ)
{
    const float fRange = farZ / (nearZ - farZ);
    return mat4(
        invTanHalfFovH,  0, 0, 0, // col1
		0, invTanHalfFovV,  0, 0, // col2
		0, 0, fRange         ,-1, // col3
		0, 0, nearZ * fRange , 0  // col4
	);
}

//  note 1) that screenCoord means the coordinate 
//  that originates at upper-leftmost.
//  2) fRange = farZ / (nearZ - farZ)
//  3) These functions are for Right-hand system
vec4 toProjCoord(vec2 screenCoord, float depth, float fRange, float nearZ)
{
    const vec3 ndcCoord = vec3(
        screenCoord.x * 2.0f - 1.0f,
        (1.0f - screenCoord.y) * 2.0f - 1.0f,
        depth
    );

    float w = nearZ * fRange / (fRange + ndcCoord.z);
    return vec4(ndcCoord * w, w);
}

vec3 toViewCoord(vec2 screenCoord, float depth, float invTanHalfFovH, float invTanHalfFovV, float fRange, float nearZ)
{
    const vec3 projCoord = toProjCoord(screenCoord, depth, fRange, nearZ).xyz;
    return vec3(
        projCoord.xy / vec2(invTanHalfFovH, invTanHalfFovV),
		(projCoord.z - nearZ * fRange) / fRange
    );
}

vec3 toWorldCoord(vec2 screenCoord, float depth, mat4 view, float invTanHalfFovH, float invTanHalfFovV, float fRange, float nearZ)
{
    const vec3 viewCoord = toViewCoord(screenCoord, depth, invTanHalfFovH, invTanHalfFovV, fRange, nearZ).xyz;
    return (inverse(view) * vec4(viewCoord, 1)).xyz;
}

//  transform projection depth value to view space value
float toViewDepth(float projDepth, float nearPlane, float farPlane)
{
    return -nearPlane * farPlane / (projDepth * (nearPlane - farPlane) + farPlane);
}