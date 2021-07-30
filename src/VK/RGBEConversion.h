//  ref : https://www.graphics.cornell.edu/~bjw/rgbe/rgbe.c

/* standard conversion from float pixels to rgbe pixels */
/* note: you can remove the "inline"s if your compiler complains about it */
uint float3ToRGBE(vec3 color)
{
    uint code = 0;

    float value = max(max(color.x, color.y), color.z);
    if (value >= 1e-32) 
    {
        int exponent;
        value = frexp(value, exponent) * 256.0f / value;

        code += uint(clamp(int(color.x * value), 0, 255));
        code += uint(clamp(int(color.y * value), 0, 255)) << 8;
        code += uint(clamp(int(color.z * value), 0, 255)) << 16;
        code += uint(clamp(exponent + 128, 0, 255)) << 24;
    }

    return code;
}

/* standard conversion from rgbe to float pixels */
/* note: Ward uses ldexp(col+0.5,exp-(128+8)). However we wanted pixels */
/*       in the range [0,1] to map back into the range [0,1].           */
vec3 RGBEToFloat3(uint code)
{
    vec3 color = vec3(0);

    ivec4 rgbe = ivec4(
        (code & 255),
        (code & (255 << 8)) >> 8,
        (code & (255 << 16)) >> 16,
        (code & (255 << 24)) >> 24
    );

    if (rgbe.w > 0) {   /*nonzero pixel*/
        float f = ldexp(1.0f, rgbe.w - 128 + 8);
        color.x = rgbe.x * f;
        color.y = rgbe.y * f;
        color.z = rgbe.z * f;
    }

    return color;
}