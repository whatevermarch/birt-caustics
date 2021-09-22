struct TransformParams
{
    mat4 view;
    vec4 position;
    float invTanHalfFovH;
    float invTanHalfFovV;
    float nearPlane;
    float farPlane;
};

//  transform projection depth value to view space value
float toViewDepth(float projDepth, float nearPlane, float farPlane)
{
    return -nearPlane * farPlane / (projDepth * (nearPlane - farPlane) + farPlane);
}