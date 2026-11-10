#include "PCH.h"
#include "TransformRows.h"
#include "Utils.h"

TransformRows CollectTransformRows(ObjectManager* om, SceneData* scene)
{
	TransformRows rows;
	om->ForEachArchetype<Positions, Renderable>(scene,
		[&](ComponentArray<Positions, void>* positions, ComponentArray<Renderable, void>* renderables)
		{
			rows.bases.push_back({ &renderables->data, rows.count });
			rows.count += safe_u32(positions->size());
		});
	return rows;
}

std::optional<uint32_t> TransformRowOf(const TransformRows& rows, SoAElement<Renderable> renderable)
{
	const auto row_base = std::find_if(rows.bases.begin(), rows.bases.end(),
		[&](const TransformRowBase& archetype) { return archetype.column == renderable.soa; });
	if (row_base == rows.bases.end()) return std::nullopt;
	return row_base->base + safe_u32(renderable.index);
}
