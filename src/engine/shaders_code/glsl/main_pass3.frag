#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec3 v_worldPos;
layout(location = 2) in vec3 v_worldNormal;
layout(location = 3) in vec3 v_worldTangent;
layout(location = 4) in vec3 v_worldBitangent;
layout(location = 5) in vec4 v_lightSpacePos[6];

layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2DArrayShadow u_shadowMapFlat;

layout(set = 2, binding = 1) uniform sampler2DArray u_albedo;
layout(set = 2, binding = 2) uniform sampler2DArray u_normal;

struct Light {
    vec4 position_radius;
    vec4 direction_angle;
    vec4 color_power;
    ivec4 light_info;
};

layout(std430, set = 2, binding = 3) buffer LightBlock {
    Light lights[];
};

struct TextureData {
    uvec4 data; // x = packed uv_offset, y = packed uv_scale, z = layer, w = pad
};

layout(set = 3, binding = 0, std140) uniform TextureUVLBlock {
    TextureData textures[4];
};

vec4 sampleAtlas(sampler2DArray atlas, uint slot, vec2 uv) {
    vec2 offset = unpackUnorm2x16(textures[slot].data.x);
    vec2 scale  = unpackUnorm2x16(textures[slot].data.y);
    uint layer  = textures[slot].data.z;
    return texture(atlas, vec3(uv * scale + offset, float(layer)));
}

vec3 computeNormal() {
    mat3 TBN = mat3(
        normalize(v_worldTangent),
        normalize(v_worldBitangent),
        normalize(v_worldNormal)
    );
    vec3 n = sampleAtlas(u_normal, 1, v_uv).rgb * 2.0 - 1.0;
    n.x = -n.x;
    return normalize(TBN * n);
}

const float AMBIENT   = 0.75;
const float EDGE_SOFT = 0.30;

float computeSpotLight(vec3 fragPos, vec3 normal, Light light)
{
    vec3  lightPos   = light.position_radius.xyz;
    float nearRadius = light.position_radius.w;
    vec3  lightDir   = normalize(light.direction_angle.xyz);
    float tanHalf    = abs(light.direction_angle.w);
    float power      = light.color_power.a;

    vec3  v = fragPos - lightPos;
    float L = dot(v, lightDir);
    if (L < 0.0) return 0.0;

    float coneRadius  = nearRadius + L * tanHalf;
    float distToAxis  = length(v - lightDir * L);
    float edge1       = coneRadius;
    float edge0       = coneRadius * (1.0 - EDGE_SOFT);
    float radiusAtten = smoothstep(edge1, edge0, distToAxis);
    if (radiusAtten <= 0.0) return 0.0;

    float dist        = length(v);
    float attenuation = 1.0 / (1.0 + 0.22 * dist + 0.2 * dist * dist);
    vec3  lDir        = normalize(lightPos - fragPos);
    float NdotL       = dot(normal, lDir);

    const float wrap  = 0.4;
    float diffuse     = max((NdotL + wrap) / (1.0 + wrap), 0.0);

    return diffuse * attenuation * radiusAtten * power;
}

float computeSphereLight(vec3 fragPos, vec3 normal, Light light)
{
    vec3  C     = light.position_radius.xyz;
    float R     = light.position_radius.w;
    float power = light.color_power.a;

    vec3  toC  = C - fragPos;
    float dist = length(toC);
    float d0   = max(dist - R, 0.0);
    float attenuation = 1.0 / (1.0 + 0.22 * d0 + 0.2 * d0 * d0);

    if (dist <= R) return 1.0 * attenuation * power;

    vec3  L        = toC / dist;
    float sinAlpha = clamp(R / dist, 0.0, 1.0);
    float cosAlpha = sqrt(max(1.0 - sinAlpha * sinAlpha, 0.0));
    float NL       = dot(normalize(normal), L);

    vec3 D;
    if (NL >= cosAlpha) {
        D = normalize(normal);
    } else {
        vec3  T    = normalize(normal) - L * NL;
        float tLen = length(T);
        D = tLen < 1e-6 ? L : normalize(L * cosAlpha + (T / tLen) * sinAlpha);
    }

    float dotL = max(dot(normalize(normal), D), 0.0) * 0.5 + 0.5;
    return dotL * attenuation * power;
}

float computeShadowSpot(int cameraIndex)
{
    vec4  lsp = v_lightSpacePos[cameraIndex];
    vec3  ndc = lsp.xyz / lsp.w;
    vec2  uv  = vec2(ndc.x * 0.5 + 0.5, 1.0 - (ndc.y * 0.5 + 0.5));
    float z   = ndc.z;

    if (any(isnan(ndc)) || any(isinf(ndc)) ||
        uv.x < 0.0 || uv.x > 1.0 ||
        uv.y < 0.0 || uv.y > 1.0 ||
        z < -0.01  || z > 1.01)
        return 1.0;

    return texture(u_shadowMapFlat, vec4(uv, float(cameraIndex), z));
}

int getCubeFace(vec3 dir) {
    vec3 a = abs(dir);
    if (a.x >= a.y && a.x >= a.z) return dir.x > 0.0 ? 0 : 1;
    if (a.y >= a.x && a.y >= a.z) return dir.y > 0.0 ? 2 : 3;
    return dir.z > 0.0 ? 4 : 5;
}

float computeShadowSphere(vec3 fragPos, vec3 lightPos, int cameraOffset)
{
    vec3 dir = fragPos - lightPos;
    int face = getCubeFace(dir);
    
    vec4 lsp = v_lightSpacePos[face]; // уже вычислено в vert
    if (lsp.w <= 0.0) return 1.0;
    
    vec3 ndc = lsp.xyz / lsp.w;
    vec2 uv = vec2(ndc.x * 0.5 + 0.5, 1.0 - (ndc.y * 0.5 + 0.5));
    float z = clamp(ndc.z - 0.0001, 0.0, 1.0);
    
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return 1.0;
    
    return texture(u_shadowMapFlat, vec4(uv, float(cameraOffset + face), z));
}

void main()
{
    vec3 n         = computeNormal();
    vec4  albedoSample = sampleAtlas(u_albedo, 0, v_uv);
    vec3  baseColor    = albedoSample.rgb;
    float alpha        = albedoSample.a;
    vec3 color     = baseColor * AMBIENT;

    for (int i = 0; i < lights.length(); ++i) {
        int   type         = lights[i].light_info.x;
        int   cameraOffset = lights[i].light_info.y;
        float intensity    = 0.0;

        if (type == 0) {
            intensity = computeSpotLight(v_worldPos, n, lights[i]);
            if (cameraOffset >= 0)
                intensity *= computeShadowSpot(cameraOffset);
        }
        else if (type == 1) {
            intensity = computeSphereLight(v_worldPos, n, lights[i]);
            if (cameraOffset >= 0)
                intensity *= computeShadowSphere(v_worldPos, lights[i].position_radius.xyz, cameraOffset);
        }
        else continue;

        if (intensity <= 0.0) continue;
        color += baseColor * lights[i].color_power.rgb * intensity;
    }

    out_color = vec4(color, alpha);
}