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
//  set 0 : uniform data
//--------------------------------------------------------------------------------------

struct AggregatorParams
{
    vec4 weights;

    uint imgWidth;
    uint imgHeight;

    float paddings[2];
};
layout (std140, binding = ID_Params) uniform Params 
{
    AggregatorParams u_params;
};

//  ToDo : find a way to dynamically change the format according to the input target
layout (rgba16f, binding = ID_Target) uniform image2D img_target;

#ifdef ID_FX0
layout (binding = ID_FX0) uniform sampler2D u_fx0;
#endif

#ifdef ID_FX1
layout (binding = ID_FX1) uniform sampler2D u_fx1;
#endif

#ifdef ID_FX2
layout (binding = ID_FX2) uniform sampler2D u_fx2;
#endif

//--------------------------------------------------------------------------------------
//  main function
//--------------------------------------------------------------------------------------

void main()
{
    ivec2 coords = ivec2(gl_GlobalInvocationID.xy);
    vec2 normCoords = vec2(coords) / vec2(u_params.imgWidth, u_params.imgHeight);

    vec4 targetColor = imageLoad(img_target, coords).rgba;
    targetColor.xyz *= u_params.weights.x;

#ifdef ID_FX0
    targetColor.xyz += texture(u_fx0, normCoords).rgb * u_params.weights.y;
#endif

#ifdef ID_FX1
    targetColor.xyz += texture(u_fx1, normCoords).rgb * u_params.weights.z;
#endif

#ifdef ID_FX2
    targetColor.xyz += texture(u_fx2, normCoords).rgb * u_params.weights.w;
#endif

    imageStore(img_target, coords, targetColor);
}
