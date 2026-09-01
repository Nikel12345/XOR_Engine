#include "PCH.h"
#include "LightDataModule.h"
#include "BaseComponents.h"
#include "Utils.h"
#include "BufferManager.h"
#include "CameraStruct.h"
#include "ObjectManager.h"

LightDataModule::LightDataModule()
{
}

uint32_t LightDataModule::CalculateLightSize(ObjectManager* om, SceneData* scene)
{
    total_size = 0;

    om->ForEachArchetype<Positions, SpotLightComponent>(
        scene,
        [&](ComponentArray<Positions, void>* posArr,
            ComponentArray<SpotLightComponent, void>*)
        {
            total_size += safe_u32(posArr->size()) * sizeof(LightLayout);
        }
    );
    om->ForEachArchetype<Positions, SphereLightComponent>(
        scene,
        [&](ComponentArray<Positions, void>* posArr,
            ComponentArray<SphereLightComponent, void>*)
        {
            total_size += safe_u32(posArr->size()) * sizeof(LightLayout);
        }
    );
    // Directional: позиции нет, считаем по самому компоненту. Один LightLayout на источник.
    om->ForEachArchetype<DirectLightComponent>(
        scene,
        [&](ComponentArray<DirectLightComponent, void>* arr)
        {
            total_size += safe_u32(arr->size()) * sizeof(LightLayout);
        }
    );

    return total_size;
}

void LightDataModule::StoreLightData(BufferManager* bm, UploadTask* task, ObjectManager* om, SceneData* scene) {
    int spot_light_cameras = 1;
    int offset = 0;
    int no_camera = -1;
    om->ForEach<Positions, SpotLightComponent>(scene,
        [&](Entity e, SoAElement<Positions> pos_el, SpotLightComponent& light) {
            Positions& P = pos_el.container();
            size_t i = pos_el.i();

            light.light_data.ResolveDistance();

			LightLayout light_layout{};
			light_layout.x = P.w[i];
			light_layout.y = P.d[i];
			light_layout.z = P.h[i];
			light_layout.w = light.light_data.source_radius;
			light_layout.dir_x = light.light_data.dir_x;
			light_layout.dir_y = light.light_data.dir_y;
			light_layout.dir_z = light.light_data.dir_z;
			light_layout.angle_tan = light.light_data.source_angle;
			light_layout.r = light.light_data.r;
			light_layout.g = light.light_data.g;
			light_layout.b = light.light_data.b;
			light_layout.power = light.light_data.power;
            light_layout.max_range = light.light_data.GetMaxDistance();

            if (om->Has<ShadowCasterComponent>(scene, e)) {
                light_layout.type = static_cast<int>(LightTypes::SPOT);
                light_layout.offset = offset;
				light_layout.padding = 0;
                offset += spot_light_cameras;
            }
            else {
                light_layout.type = static_cast<int>(LightTypes::SPOT);
                light_layout.offset = no_camera;
                light_layout.padding = 0;
            };
			bm->UploadToTransferBuffer(task, sizeof(LightLayout), &light_layout);
        });

	int sphere_light_cameras = 6;
    om->ForEach<Positions, SphereLightComponent>(scene,
        [&](Entity e, SoAElement<Positions> pos_el, SphereLightComponent& light) {
            Positions& P = pos_el.container();
            size_t i = pos_el.i();

            light.light_data.ResolveDistance();

			LightLayout light_layout{};
			light_layout.x = P.w[i];
			light_layout.y = P.d[i];
			light_layout.z = P.h[i];
			light_layout.w = light.light_data.source_radius;
			light_layout.dir_x = 0.0f;
			light_layout.dir_y = 0.0f;
			light_layout.dir_z = 0.0f;
			light_layout.angle_tan = 0.0f;
			light_layout.r = light.light_data.r;
			light_layout.g = light.light_data.g;
			light_layout.b = light.light_data.b;
            light_layout.power = light.light_data.power;
            light_layout.max_range = light.light_data.GetMaxDistance();

            if (om->Has<ShadowCasterComponent>(scene, e)) {
                light_layout.type = static_cast<int>(LightTypes::SPHERE);
                light_layout.offset = offset;
                light_layout.padding = 0;
                offset += sphere_light_cameras;
            }
            else {
                light_layout.type = static_cast<int>(LightTypes::SPHERE);
                light_layout.offset = no_camera;
                light_layout.padding = 0;
			};
			bm->UploadToTransferBuffer(task, sizeof(LightLayout), &light_layout);
        });

    // Directional: третий блок в том же порядке spot→sphere→direct, чтобы сквозной
    // offset/camera_index оставался согласован с StoreLightCameras и теневым проходом.
    om->ForEach<DirectLightComponent>(scene,
        [&](Entity e, DirectLightComponent& light) {
            DirectLightComponent::DirectLightData& d = light.light_data;

            glm::vec3 dir = glm::normalize(glm::vec3(d.dir_x, d.dir_y, d.dir_z));

            LightLayout light_layout{};
            // Позиции у directional нет — x/y/z не используются шейдером.
            light_layout.x = 0.0f;
            light_layout.y = 0.0f;
            light_layout.z = 0.0f;
            // w (source_radius) простаивает у directional → переиспользуем под число каскадов.
            light_layout.w = static_cast<float>(d.cascade_count);
            light_layout.dir_x = dir.x;
            light_layout.dir_y = dir.y;
            light_layout.dir_z = dir.z;
            light_layout.angle_tan = 0.0f;
            light_layout.r = d.r;
            light_layout.g = d.g;
            light_layout.b = d.b;
            light_layout.power = d.power;
            // Глубина у directional нормируется per-cascade (ndc.z каждого каскада),
            // поэтому общий max_range шейдеру не нужен.
            light_layout.max_range = 0.0f;

            if (om->Has<ShadowCasterComponent>(scene, e)) {
                light_layout.type = static_cast<int>(LightTypes::DIRECT);
                light_layout.offset = offset;
                light_layout.padding = 0;
                offset += d.cascade_count;
            }
            else {
                light_layout.type = static_cast<int>(LightTypes::DIRECT);
                light_layout.offset = no_camera;
                light_layout.padding = 0;
            };
            bm->UploadToTransferBuffer(task, sizeof(LightLayout), &light_layout);
        });
}

// Размер буфера LightCameras слота + СЛЕПОК его теневых камер (snapshots[slot]) одним
// перечислением. ОБЯЗАН идти теми же тремя запросами (фильтр ShadowCasterComponent) и в том
// же порядке spot→sphere→direct, что StoreLightCameras: индекс в cams = camera_index =
// позиция камеры в буфере. Совпадение больше не инвариант «трёх одинаковых ForEach по
// файлам» — теневой проход и каллинг читают эту таблицу, а не ECS.
uint32_t LightDataModule::CalculateLightCamerasSize(uint8_t slot) const
{
    return safe_u32(snapshots[slot].cams.size()) * safe_u32(sizeof(LightCamera));
}

void LightDataModule::StampShadowCameras(ObjectManager* om, SceneData* scene, uint8_t slot)
{
    std::vector<RenderSnap::ShadowCam>& cams = snapshots[slot].cams;
    cams.clear();   // capacity переживает кадры — аллокаций в steady state нет
    if (!scene) return;

    om->ForEach<Positions, SpotLightComponent, ShadowCasterComponent>(scene,
        [&](SoAElement<Positions>, SpotLightComponent& light, ShadowCasterComponent) {
            cams.push_back({ light.light_data.GetMaxDistance(), 0,
                             static_cast<uint8_t>(light.needsUpdate ? 1 : 0) });
        });
    om->ForEach<Positions, SphereLightComponent, ShadowCasterComponent>(scene,
        [&](SoAElement<Positions>, SphereLightComponent& light, ShadowCasterComponent) {
            for (int face = 0; face < 6; ++face)
                cams.push_back({ light.light_data.GetMaxDistance(), 0,
                                 static_cast<uint8_t>(light.needsUpdate ? 1 : 0) });
        });
    // Directional: cascade_count ortho-камер на источник (per-instance, т.к. число каскадов
    // у разных источников может отличаться). max_range — per-cascade far.
    om->ForEach<DirectLightComponent, ShadowCasterComponent>(scene,
        [&](DirectLightComponent& light, ShadowCasterComponent&) {
            for (int c = 0; c < light.light_data.cascade_count; ++c)
                cams.push_back({ light.light_data.CascadeFar(c), 1,
                                 static_cast<uint8_t>(light.needsUpdate ? 1 : 0) });
        });
}

inline void StoreSpotLightCamera(BufferManager* bm, UploadTask* task, Positions& P, size_t i, SpotLightComponent::SpotLightData& light) {

    glm::vec3 position = glm::vec3(P.w[i], P.d[i], P.h[i]);

    glm::vec3 dir = glm::normalize(glm::vec3(
        light.dir_x, light.dir_y, light.dir_z));

    glm::vec3 up = (std::abs(dir.y) > 0.99f)
        ? glm::vec3(1.0f, 0.0f, 0.0f)
        : glm::vec3(0.0f, 1.0f, 0.0f);

    glm::mat4 view = glm::lookAt(position, position + dir, up);

    float fov = 2.0f * std::atan(light.source_angle);
    // near маленький: distance shadow map не зависит от точности near-плоскости,
    // зато близкие к источнику окклюдеры (пол прямо над лампой) не отсекаются.
    glm::mat4 proj = glm::perspectiveZO(
        fov, 1.0f, 0.05f, light.GetMaxDistance());

    LightCamera lc{};
    lc.view = view;
    lc.proj = proj;

    bm->UploadToTransferBuffer(task, sizeof(LightCamera), &lc);
}

static const glm::vec3 cubeDirs[6] = {
    { 1,  0,  0}, {-1,  0,  0},  // +X, -X
    { 0,  1,  0}, { 0, -1,  0},  // +Y, -Y
    { 0,  0,  1}, { 0,  0, -1}   // +Z, -Z
};
// Выводится из Vulkan spec cubemap UV convention
static const glm::vec3 cubeUps[6] = {
    { 0, 1,  0},  // +X: cam_x=-rz, cam_y=-ry ✓
    { 0, 1,  0},  // -X: cam_x=+rz, cam_y=-ry ✓
    { 0,  0,  1},  // +Y: cam_x=+rx, cam_y=+rz ✓
    { 0,  0, -1},  // -Y: cam_x=+rx, cam_y=-rz ✓
    { 0, 1,  0},  // +Z: cam_x=+rx, cam_y=-ry ✓
    { 0, 1,  0},  // -Z: cam_x=-rx, cam_y=-ry ✓
};


inline void StoreSphereLightCameras(BufferManager* bm, UploadTask* task, Positions& P, size_t i, SphereLightComponent::SphereLightData& light) {
	glm::vec3 position = glm::vec3(P.w[i], P.d[i], P.h[i]);
    glm::mat4 proj = glm::perspectiveZO(
		glm::radians(91.0f), 1.0f, 0.05f, light.GetMaxDistance());
    for (int face = 0; face < 6; ++face)
    {
        glm::mat4 view = glm::lookAt(
            position, position + cubeDirs[face], cubeUps[face]);
        LightCamera lc{};
        lc.view = view;
        lc.proj = proj;
        bm->UploadToTransferBuffer(task, sizeof(LightCamera), &lc);
	}
}

// Ortho-камера(ы) directional. Камера статична (не едет за игроком): бокс строится
// вокруг center из компонента. eye отодвинут на half_depth в сторону источника, чтобы
// near=0 не отсёк окклюдеры перед центром. orthoZO — depth [0,1], как perspectiveZO.
// Сейчас 1 каскад; под CSM здесь будет цикл по слайсам фрустума камеры.
inline void StoreDirectionalCascades(BufferManager* bm, UploadTask* task,
    DirectLightComponent::DirectLightData& d) {
    glm::vec3 dir = glm::normalize(glm::vec3(d.dir_x, d.dir_y, d.dir_z));
    glm::vec3 center = glm::vec3(d.center_x, d.center_y, d.center_z);
    glm::vec3 up = (std::abs(dir.y) > 0.99f)
        ? glm::vec3(1.0f, 0.0f, 0.0f)
        : glm::vec3(0.0f, 1.0f, 0.0f);

    // Вложенные концентрические боксы, мелкий→крупный (каскад 0 первым — шейдер берёт
    // первый содержащий). И латераль, и глубина масштабируются ratio^c: иначе пол
    // покрывается только в боковом направлении. eye отодвигается на per-cascade глубину.
    for (int c = 0; c < d.cascade_count; ++c) {
        float he    = d.CascadeExtent(c);
        float depth = d.CascadeDepth(c);
        glm::vec3 eye = center - dir * depth;
        glm::mat4 view = glm::lookAt(eye, eye + dir, up);
        glm::mat4 proj = glm::orthoZO(
            -he, he,
            -he, he,
            0.0f, 2.0f * depth);

        LightCamera lc{};
        lc.view = view;
        lc.proj = proj;
        bm->UploadToTransferBuffer(task, sizeof(LightCamera), &lc);
    }
}

void LightDataModule::StoreLightCameras(BufferManager* bm, UploadTask* task, ObjectManager* om, SceneData* scene) {
    om->ForEach<Positions, SpotLightComponent, ShadowCasterComponent>(scene,
        [&](SoAElement<Positions> pos_el, SpotLightComponent& light, ShadowCasterComponent) {
            Positions& P = pos_el.container();
            size_t i = pos_el.i();
			StoreSpotLightCamera(bm, task, P, i, light.light_data);
	});
    om->ForEach<Positions, SphereLightComponent, ShadowCasterComponent>(scene,
        [&](SoAElement<Positions> pos_el, SphereLightComponent& light, ShadowCasterComponent) {
            Positions& P = pos_el.container();
            size_t i = pos_el.i();
            StoreSphereLightCameras(bm, task, P, i, light.light_data);
	});
    om->ForEach<DirectLightComponent, ShadowCasterComponent>(scene,
        [&](DirectLightComponent& light, ShadowCasterComponent) {
            StoreDirectionalCascades(bm, task, light.light_data);
        });
}

