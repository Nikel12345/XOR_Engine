#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>

struct BufferData;
class BufferManager;

struct CameraData {
	glm::mat4 view_matrix;
	glm::mat4 projection_matrix;
};

class Camera {
public:
    Camera(float WIDTH, float HEIGHT);
    CameraData* GetViewMatrix();
    void StoreViewMatrix(BufferManager* bm, BufferData* buffer_data);
    void SetView(const glm::vec3& position, const glm::vec3& target, const glm::vec3& up);
    void MoveView(const glm::vec3& offset);
    void RotateView(float mouse_x, float mouse_y, bool lmb_down);
    void UpdateWindowSize(float width, float height);
    glm::vec3 GetPosition() const { return position; };
    glm::vec3 GetTarget() const { return target; };


private:
    CameraData view_data;
    glm::vec3 position;
    glm::vec3 target;
    glm::vec3 up;

    float yaw = -90.0f;
    float pitch = 0.0f;
    bool first_mouse = true;
    int last_x = 0, last_y = 0;
};