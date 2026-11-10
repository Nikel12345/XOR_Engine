#pragma once
#include <cstdint>
#include <optional>
#include <vector>
#include "BaseComponents.h"
#include "ObjectManager.h"

struct TransformRowBase {
	const Renderable* column;
	uint32_t          base;
};

struct TransformRows {
	std::vector<TransformRowBase> bases;
	uint32_t                      count = 0;
};

TransformRows CollectTransformRows(ObjectManager* om, SceneData* scene);
std::optional<uint32_t> TransformRowOf(const TransformRows& rows, SoAElement<Renderable> renderable);
