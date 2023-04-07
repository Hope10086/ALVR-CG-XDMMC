#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#ifdef ENABLE_MULTIVEW_EXT
    #extension GL_EXT_multiview : enable
#endif
#pragma vertex

layout (std140, push_constant) uniform buf
{
#ifdef ENABLE_MULTIVEW_EXT
    mat4 mvp[2];
#else
    mat4 mvp;
    uint ViewID;
#endif
} ubuf;

#ifdef ENABLE_MULTIVEW_EXT
    #define VS_GET_VIEW_INDEX(input) gl_ViewIndex
    #define VS_GET_VIEW_PROJ(input) input.mvp[gl_ViewIndex]
#else
    #define VS_GET_VIEW_INDEX(input) input.ViewID
    #define VS_GET_VIEW_PROJ(input) input.mvp
#endif

#ifdef ENABLE_MVP_TRANSFORM
    #define VS_MVP_TRANSFORM(input, pos) VS_GET_VIEW_PROJ(input) * vec4(pos, 1.0)
#else
    #define VS_MVP_TRANSFORM(input, pos) vec4(pos, 1.0)
#endif

layout (location = 0) in vec3 Position;
layout (location = 1) in vec2 UV;
            
layout (location = 0) out vec2 oUV;            
out gl_PerVertex
{
    vec4 gl_Position;
};

void main()
{
    vec2 ouv = UV;
    if (VS_GET_VIEW_INDEX(ubuf) > 0) {
        ouv.x += 0.5f;
    }
    oUV = ouv;
    gl_Position = VS_MVP_TRANSFORM(ubuf, Position);
    gl_Position.y = -gl_Position.y;
}
