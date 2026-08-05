#include "PCH.h"
#include "CameraStruct.h"

Camera::Camera(float width, float height,
    float fov_y,
    float near_plane,
    float far_plane)
{
    position = glm::vec3(0.0f, 0.0f, 5.0f);

    yaw = -90.0f;
    pitch = 0.0f;

    fov = fov_y;
    aspect = width / height;
    nearPlane = near_plane;
    farPlane = far_plane;

    UpdateOrientation();
}

void Camera::SetView(const glm::vec3& pos, const glm::vec3& dir, const glm::vec3& upVec)
{
    position = pos;
    forward = glm::normalize(dir); 

    up = glm::normalize(upVec);
    right = glm::normalize(glm::cross(forward, up));
    up = glm::normalize(glm::cross(right, forward));

    yaw = glm::degrees(atan2(forward.z, forward.x));
    pitch = glm::degrees(asin(forward.y));
}


void Camera::UpdateOrientation()
{
    glm::vec3 dir;
    dir.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch));
    dir.y = sin(glm::radians(pitch));
    dir.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch));

    forward = glm::normalize(dir);

    right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
    up = glm::normalize(glm::cross(right, forward));
}

void Camera::Move(const glm::vec3& offset)
{
    glm::vec3 delta =
        right * offset.x +
        up * offset.y +
        forward * offset.z;

    position += delta * moveSpeed;
}

void Camera::Rotate(float dx, float dy)
{
    yaw += dx;
    pitch += dy;

    if (pitch > 89.0f)  pitch = 89.0f;
    if (pitch < -89.0f) pitch = -89.0f;

    UpdateOrientation();
}

void Camera::RotateView(float mouse_x, float mouse_y, bool lmb_down)
{
    if (!lmb_down) {
        firstMouse = true;
        return;
    }

    if (firstMouse) {
        lastMouseX = mouse_x;
        lastMouseY = mouse_y;
        firstMouse = false;
        return;
    }

    float dx = mouse_x - lastMouseX;
    float dy = mouse_y - lastMouseY;

    lastMouseX = mouse_x;
    lastMouseY = mouse_y;

    const float sensitivity = 0.1f;
    Rotate(dx * sensitivity, -dy * sensitivity);
}

void Camera::LookAt(const glm::vec3& pos, const glm::vec3& tgt)
{
    position = pos;
    forward = glm::normalize(tgt - pos);

    right = glm::normalize(glm::cross(forward, glm::vec3(0, 1, 0)));
    up = glm::normalize(glm::cross(right, forward));

    yaw = glm::degrees(atan2(forward.z, forward.x));
    pitch = glm::degrees(asin(forward.y));
}

glm::mat4 Camera::GetView() const
{
    return glm::lookAt(position, position + forward, up);
}

glm::mat4 Camera::GetProj() const
{
    return glm::perspective(glm::radians(fov), aspect, nearPlane, farPlane);
}
