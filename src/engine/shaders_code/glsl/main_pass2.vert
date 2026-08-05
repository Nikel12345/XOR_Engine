#version 450
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec3 a_normal;
layout(location = 3) in vec3 a_tangent;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec3 v_worldPos;
layout(location = 2) out vec3 v_worldNormal;
layout(location = 3) out vec3 v_worldTangent;
layout(location = 4) out vec3 v_worldBitangent;
layout(location = 5) out vec4 v_lightSpacePos[6];

layout(set = 0, binding = 0, std430) buffer ModelMatrixBlock {
    mat4 models[];
};
layout(set = 0, binding = 1, std430) buffer PositionIndexBuffer {
    int posIndex[];
};
layout(set = 0, binding = 2, std140) buffer Camera {
    mat4 view;
    mat4 proj;
};
struct LightCamera {
    mat4 view;
    mat4 proj;
};
layout(set = 0, binding = 3, std430) buffer LightCameras {
    LightCamera cameras[];
};
void main()
{
    mat4 modelMatrix = models[posIndex[gl_InstanceIndex]];
    vec4 worldPos = modelMatrix * vec4(a_pos, 1.0);
   
    mat3 normalMatrix = mat3(modelMatrix);
    vec3 worldNormal = normalize(normalMatrix * a_normal);
    vec3 worldTangent = normalize(normalMatrix * a_tangent);
    vec3 worldBitangent = normalize(cross(worldNormal, worldTangent));
   
    gl_Position = proj * view * worldPos;
   
    v_uv = a_uv;
    v_worldPos = worldPos.xyz;
    v_worldNormal = worldNormal;
    v_worldTangent = worldTangent;
    v_worldBitangent = worldBitangent;
   
    // Вычисляем light-space позиции для всех камер
    for (int i = 0; i < 6; i++) {
        v_lightSpacePos[i] = cameras[i].proj * cameras[i].view * worldPos;
    }
}