#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

class CameraManager;

struct CameraData {
    glm::mat4 view;
    glm::mat4 proj;
};

class Camera {
    friend class CameraManager;
public:
    static constexpr float DEFAULT_FOV_Y = 45.0f;
    static constexpr float DEFAULT_NEAR_PLANE = 0.01f;
    static constexpr float DEFAULT_FAR_PLANE = 5000.0f;

    // Пределы скорости полёта. Дальше расширять нет смысла: ограничивает уже не float как
    // таковой (1e±38), а точность ПОЗИЦИИ в Move — у 1e6 шаг младшего бита позиции ~0.06,
    // ниже 1e-6 приращение тонет в округлении на сколь-нибудь удалённой позиции.
    static constexpr float MIN_MOVE_SPEED = 1e-6f;
    static constexpr float MAX_MOVE_SPEED = 1e6f;
    static constexpr float SPEED_STEP     = 2.0f;   // множитель на один «щелчок» (delta = ±1)

    Camera(float width, float height,
        float fov_y,
        float near_plane,
        float far_plane);

    void Move(const glm::vec3& offset);
    void Rotate(float dx, float dy);
    void RotateView(float mouse_x, float mouse_y, bool lmb_down);
	// Мультипликативно (×SPEED_STEP^delta), НЕ аддитивно: диапазон в 12 декад инкрементом
	// непроходим — у нуля любой фиксированный шаг груб, у миллиона незаметен. ×2 на щелчок
	// даёт равномерный по ощущению разгон: весь диапазон — ~40 щелчков колеса.
	void SpeedChange(float delta) { moveSpeed = glm::clamp(moveSpeed * glm::pow(SPEED_STEP, delta), MIN_MOVE_SPEED, MAX_MOVE_SPEED); }
    void LookAt(const glm::vec3& pos, const glm::vec3& tgt);
    void SetView(const glm::vec3& pos, const glm::vec3& dir, const glm::vec3& upVec);
	void SetPosition(const glm::vec3& pos) { position = pos; }

    glm::mat4 GetView() const;
    glm::mat4 GetProj() const;

    glm::vec3 GetPosition() const { return position; }
    glm::vec3 GetTarget()   const { return forward; }
    glm::vec3 GetForward()  const { return forward; }

    void SetAspect(float aspectRatio) { aspect = aspectRatio; }
    void SetFOV(float f) { fov = f; }
    void SetPlanes(float n, float f) { nearPlane = n; farPlane = f; }

private:
    void UpdateOrientation();
    
    glm::vec3 position;
    glm::vec3 forward;
    glm::vec3 up;
    glm::vec3 right;

    float yaw;
    float pitch;

    float fov;
    float aspect;
    float nearPlane;
    float farPlane;
    float moveSpeed = 1.0f;

    bool firstMouse = true;
    float lastMouseX = 0.0f;
    float lastMouseY = 0.0f;

    bool active = true;
};
