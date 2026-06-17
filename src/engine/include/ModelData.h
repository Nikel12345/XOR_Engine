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
};

struct ModelData {
    std::vector<SubMeshData> submeshes;
};