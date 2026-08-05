#version 450

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec3 a_normal;
layout(location = 3) in vec3 a_tangent;

layout(set = 0, binding = 0, std430) buffer ModelMatrixBlock {
    mat4 models[];
};

layout(set = 0, binding = 1, std430) buffer PositionIndexBuffer {
    int posIndex[];
};

struct LightCamera {
    mat4 view;
    mat4 proj;
};

layout(set = 0, binding = 2, std430) buffer LightCameras {
    LightCamera cameras[];
};

layout(set = 1, binding = 0, std140) uniform CurrentCameraUBO {
    int currentCameraIndex;
};

void main()
{
    mat4 modelMatrix = models[posIndex[gl_InstanceIndex]];
    vec4 worldPos = modelMatrix * vec4(a_pos, 1.0);

    gl_Position = cameras[currentCameraIndex].proj *
                  cameras[currentCameraIndex].view *
                  worldPos;
}