#include "PCH.h"
#include "PositionStructure.h"

float u_size = 1.0f / 1.0f;
float v_size = 1.0f / 1.0f;

int frame_x = 0;
int frame_y = 0;

float u0 = frame_x * u_size;
float v0 = frame_y * v_size;
float u1 = u0 + u_size;
float v1 = v0 + v_size;

const std::vector<Uint16> IndicesCube = {
    // Front face
    0, 1, 2,  2, 3, 0,
    // Back face
    4, 6, 5,  4, 7, 6,
    // Left face
    8, 9, 10,  10, 11, 8,
    // Right face
    12, 13, 14,  14, 15, 12,
    // Top face
    16, 17, 18,  18, 19, 16,
    // Bottom face
    20, 21, 22,  22, 23, 20,
};


const std::vector<Uint16> IndicesSquare = {
    0, 1, 2,
    2, 3, 0
};


void UpdateRotationByMouse(float mouse_x, float mouse_y, float &angle_x, float &angle_y, std::vector<MatrixParams>& current_params)
{
	static float last_x = 0, last_y = 0;
    static bool first_mouse = true;
    if (first_mouse) {
        last_x = mouse_x;
        last_y = mouse_y;
        first_mouse = false;
    }
    float sensitivity = 0.2f;
	float dx = mouse_x - last_x;
	float dy = mouse_y - last_y;
    angle_y += dx * sensitivity;
    angle_x += dy * sensitivity;
    if (angle_x > 89.0f)  angle_x = 89.0f;
    if (angle_x < -89.0f) angle_x = -89.0f;
    for (auto& param : current_params) {
        param.angleX = glm::radians(angle_x);
        param.angleY = glm::radians(angle_y);
        param.angleZ = 0.0f;
	}

    last_x = mouse_x;
	last_y = mouse_y;
}

constexpr int stacks = 12;
constexpr int slices = 24;
constexpr float radius = 0.8f;
