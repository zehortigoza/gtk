
#version 450

#include "clip.vert.glsl"

layout(location = 0) in vec4 inRect;
layout(location = 1) in vec4 inTexRect;
layout(location = 2) in float inRadius;
layout(location = 3) in uvec2 inTexId;

layout(location = 0) out vec2 outPos;
layout(location = 1) out flat vec2 outSize;
layout(location = 2) out vec2 outTexCoord;
layout(location = 3) out flat float outRadius;
layout(location = 4) out flat uvec2 outTexId;

vec2 offsets[6] = { vec2(0.0, 0.0),
                    vec2(1.0, 0.0),
                    vec2(0.0, 1.0),
                    vec2(0.0, 1.0),
                    vec2(1.0, 0.0),
                    vec2(1.0, 1.0) };

void main() {
  vec4 rect = clip (inRect);
  vec2 pos = rect.xy + rect.zw * offsets[gl_VertexIndex];
  gl_Position = push.mvp * vec4 (push.scale * pos, 0.0, 1.0);

  outPos = pos;
  outSize = inRect.zw;

  vec4 texrect = vec4((rect.xy - inRect.xy) / inRect.zw,
                      rect.zw / inRect.zw);
  texrect = vec4(inTexRect.xy + inTexRect.zw * texrect.xy,
                 inTexRect.zw * texrect.zw);
  outTexCoord = texrect.xy + texrect.zw * offsets[gl_VertexIndex];
  outTexId = inTexId;
  outRadius = inRadius;
}
