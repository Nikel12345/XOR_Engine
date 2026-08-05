#include "Camera.h"
#include "BufferManager.h"

Camera::Camera(float WIDHT, float HEIGHT) {
    float aspect = WIDHT / HEIGHT;
    position = glm::vec3(3.0f, 5.0f, 3.0f);
    target = glm::vec3(0.0f, 0.0f, 0.0f);
    up = glm::vec3(0.0f, 1.0f, 0.0f);
    view_data.view_matrix = glm::lookAt(position, target, up);
    view_data.projection_matrix = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 5000.0f);
}

void Camera::SetView(const glm::vec3& pos, const glm::vec3& tgt, const glm::vec3& upVec) {
    position = pos;
    target = tgt;
    up = upVec;
    view_data.view_matrix = glm::lookAt(position, target, up);
}

void Camera::MoveView(const glm::vec3& offset) {
    position += offset;
    target += offset;
    view_data.view_matrix = glm::lookAt(position, target, up);
}

void Camera::RotateView(float mouse_x, float mouse_y, bool lmb_down)
{
    static bool first_mouse = true;
    static float last_x = 0, last_y = 0;

    if (!lmb_down) {
        first_mouse = true;
        return;
    }

    if (first_mouse) {
        last_x = mouse_x;
        last_y = mouse_y;
        first_mouse = false;
    }

    float sensitivity = 0.2f;
    float dx = mouse_x - last_x;
    float dy = mouse_y - last_y;

    yaw += dx * sensitivity;
    pitch -= dy * sensitivity;

    if (pitch > 89.0f)  pitch = 89.0f;
    if (pitch < -89.0f) pitch = -89.0f;

    glm::vec3 direction;
    direction.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch));
    direction.y = sin(glm::radians(pitch));
    direction.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch));
    target = position + glm::normalize(direction);

    view_data.view_matrix = glm::lookAt(position, target, up);

    last_x = mouse_x;
    last_y = mouse_y;
}

void Camera::UpdateWindowSize(float width, float height)
{
    float aspect = width / height;
    view_data.projection_matrix = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 100.0f);
}


CameraData* Camera::GetViewMatrix() {
    return &view_data;
}

void Camera::StoreViewMatrix(BufferManager* bm, BufferData* buffer_data)
{
    size_t sz = sizeof(CameraData);
    bm->uploadToGPUBuffer(buffer_data, glm::value_ptr(view_data.view_matrix), safe_u32(sz));
}
