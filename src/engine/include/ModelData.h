#pragma once
#include <vector>
#include <SDL3/SDL_stdinc.h>   // Uint32
#include <glm/glm.hpp>         // glm::vec4 (раньше приходили из PCH)

struct SubMeshData {
    Uint32 vertexOffset = 0;
    Uint32 indexOffset = 0;
    Uint32 vertexCount = 0;
    Uint32 indexCount = 0;
	// ������ ��������� � ������� ���������� ������, ������� ������������ ��� ������� ����� �������. (�������� ������������ � MaterialComponent ��� ���������, ��� ��� ��������)
	// Index of the material in the model's materials array, which is used for rendering this submesh. (See documentation for MaterialComponent for understanding how it works)
	uint32_t material_index = 0;
    glm::vec4 sphere;
    glm::vec3 aabb_center = glm::vec3(0.0f);  // центр локального AABB сабмеша (по min/max вершин)
    glm::vec3 aabb_half   = glm::vec3(0.0f);  // полу-размеры локального AABB
};

// Точка отсчёта (пивот) модели. Запекается в вершины один раз при CreateModel:
// геометрия сдвигается так, чтобы выбранный угол/центр локального AABB попал в origin.
// Keep — не сдвигать (поведение по умолчанию, обратная совместимость).
// L/R = X min/max, B/T = Y min/max (Bottom/Top), B/F = Z min/max (Back/Front).
enum class AnchorShift { Keep, Center, LBB, RBB, LTB, RTB, LBF, RBF, LTF, RTF };

struct ModelData {
    std::vector<SubMeshData> submeshes;
    // Как сдвинут пивот. Читается только в момент запекания (CreateModel);
    // дальше — информативная метка, рендер её не использует.
    AnchorShift anchor = AnchorShift::Keep;
};