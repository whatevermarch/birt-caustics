#pragma once

struct ISRTTransform
{
    XMMATRIX view;
    XMVECTOR position;
    float invTanHalfFovH;
    float invTanHalfFovV;
    float nearPlane;
    float farPlane;
};
