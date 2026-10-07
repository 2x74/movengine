#pragma once

namespace render {

// world shader for the game and the editor. uLightingMode:
//   0 = no light data, the old fixed directional shading
//   1 = baked vrad lightmaps (a .bsp), sampled from uLightmap
//   2 = live light entities (a .vmf being playtested / edited): sun + ambient + up to kMaxPointLights point lights, no shadows
inline constexpr int kMaxPointLights = 32;

inline constexpr const char* kTexturedVertexSrc = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec2 aLightmapUV;

uniform mat4 uViewProj;

out vec3 vNormal;
out vec2 vUV;
out vec2 vLightmapUV;
out vec3 vWorldPos;

void main() {
    vNormal = aNormal;
    vUV = aUV;
    vLightmapUV = aLightmapUV;
    vWorldPos = aPosition;
    gl_Position = uViewProj * vec4(aPosition, 1.0);
}
)glsl";

inline constexpr const char* kTexturedFragmentSrc = R"glsl(
#version 330 core
in vec3 vNormal;
in vec2 vUV;
in vec2 vLightmapUV;
in vec3 vWorldPos;
out vec4 FragColor;

uniform sampler2D uTexture;
uniform sampler2D uLightmap;
uniform int uLightingMode;
uniform float uAlpha;

uniform vec3 uSunDirection;  // the way sunlight travels
uniform vec3 uSunColor;
uniform vec3 uAmbient;
uniform int uPointCount;
uniform vec3 uPointPos[32];
uniform vec3 uPointColor[32];
uniform float uPointFifty[32];

// fog. with no uFogStart this is the draw distance fade: past 70% of uFogEnd the world blends into uFogColor so the far plane cutoff isn't a hard line. a map's own env_fog_controller sets uFogStart and takes over. uFogEnd 0 = off
uniform float uFogEnd;
uniform float uFogStart;
uniform float uFogMaxDensity;
uniform vec3 uFogColor;
// mat_brightness: scales the lit result before it goes back to gamma. an unset uniform reads 0 and callers that draw with this shader without setting it (the editor) must not render black, so 0 means 1
uniform float uExposure;
// > 0 turns on $alphatest: texels below this are not drawn at all. Foliage
// and fences are a flat quad with their shape in the alpha channel.
uniform float uAlphaTestRef;
uniform vec3 uViewPos;

// textures and light colours are authored in gamma space, multiplying two of them together isn't the same as lighting a surface and the error lands hardest in the midtones, which is most of a map. so undo the encoding on the way in, do everything linear, re-encode once at the end
const float kGamma = 2.2;
// must match LightmapAtlas::kLightmapRange in bsp_loader.cpp
const float kLightmapRange = 2.0;
vec3 toLinear(vec3 c) { return pow(max(c, 0.0), vec3(kGamma)); }

void main() {
    vec4 texel = texture(uTexture, vUV);
    if (uAlphaTestRef > 0.0 && texel.a < uAlphaTestRef) discard;
    vec3 albedo = toLinear(texel.rgb);
    vec3 n = normalize(vNormal);
    vec3 light;
    if (uLightingMode == 1) {
        light = toLinear(texture(uLightmap, vLightmapUV).rgb) * kLightmapRange;
    } else if (uLightingMode == 2) {
        light = toLinear(uAmbient) + toLinear(uSunColor) * max(dot(n, -uSunDirection), 0.0);
        for (int i = 0; i < uPointCount; ++i) {
            vec3 toLight = uPointPos[i] - vWorldPos;
            float d = length(toLight);
            // half brightness at the light's 50% distance, like vrad's quadratic falloff
            float atten = 1.0 / (1.0 + (d / uPointFifty[i]) * (d / uPointFifty[i]));
            light += toLinear(uPointColor[i]) * atten * max(dot(n, toLight / max(d, 0.001)), 0.0);
        }
    } else {
        vec3 lightDir = normalize(vec3(0.4, 0.5, 0.8));
        light = vec3(0.35 + 0.65 * max(dot(n, lightDir), 0.0));
    }
    vec3 color = albedo * light;
    if (uFogEnd > 0.0) {
        float fogStart = uFogStart > 0.0 ? uFogStart : uFogEnd * 0.7;
        float density = uFogMaxDensity > 0.0 ? uFogMaxDensity : 1.0;
        float fog = clamp((distance(vWorldPos, uViewPos) - fogStart) / max(uFogEnd - fogStart, 1.0), 0.0, 1.0);
        color = mix(color, toLinear(uFogColor), fog * density);
    }
    float exposure = uExposure > 0.0 ? uExposure : 1.0;
    color = pow(max(color * exposure, 0.0), vec3(1.0 / kGamma));
    // uAlpha is the caller's own fade (tool brushes); the texture's own
    // alpha carries $translucent.
    FragColor = vec4(color, uAlpha * texel.a);
}
)glsl";

inline constexpr const char* kDebugBoxVertexSrc = R"glsl(
#version 330 core
layout(location = 0) in vec3 aPosition;

uniform mat4 uViewProj;
uniform mat4 uModel;

void main() {
    gl_Position = uViewProj * uModel * vec4(aPosition, 1.0);
}
)glsl";

inline constexpr const char* kDebugBoxFragmentSrc = R"glsl(
#version 330 core
out vec4 FragColor;

uniform vec4 uColor;

void main() {
    FragColor = uColor;
}
)glsl";

}  // namespace render
