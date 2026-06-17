#include "PCH.h"
#include "LightDataModule.h"
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

    return total_size;
}

void LightDataModule::StoreLightData(BufferManager* bm, UploadTask* task, ObjectManager* om, SceneData* scene) {
    int spot_light_cameras = 1;
    int offset = 0;
    int no_camera = -1;
    om->ForEach<Positions, SpotLightComponent>(scene,
        [&](Entity e, SoAElement<Positions> pos_el, SpotLightComponent& light) {
        //SDL_Log("Light entity %u: offset=%d, layer=%d", e, offset, spot_layer_index);
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
                light_layout.type = LightTypes::SPOT;
                light_layout.offset = offset;
				light_layout.padding = 0;
                offset += spot_light_cameras;
            }
            else {
                light_layout.type = LightTypes::SPOT;
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
                light_layout.type = LightTypes::SPHERE;
                light_layout.offset = offset;
                light_layout.padding = 0;
                offset += sphere_light_cameras;
            }
            else {
                light_layout.type = LightTypes::SPHERE;
                light_layout.offset = no_camera;
                light_layout.padding = 0;
			};
			bm->UploadToTransferBuffer(task, sizeof(LightLayout), &light_layout);
        });
}

uint32_t LightDataModule::CalculateLightCamerasSize(ObjectManager* om, SceneData* scene)
{
    uint32_t cameraCount = 0;

    om->ForEachArchetype<Positions, SpotLightComponent, ShadowCasterComponent>(scene,
        [&](ComponentArray<Positions, void>* posArr,
            ComponentArray<SpotLightComponent, void>*,
            ComponentArray<ShadowCasterComponent, void>*)
        {
            cameraCount += safe_u32(posArr->size());
        }
	);
    om->ForEachArchetype<Positions, SphereLightComponent, ShadowCasterComponent>(scene,
        [&](ComponentArray<Positions, void>* posArr,
            ComponentArray<SphereLightComponent, void>*,
            ComponentArray<ShadowCasterComponent, void>*)
        {
            cameraCount += safe_u32(posArr->size()) * 6;
        }
    );
    return cameraCount * sizeof(LightCamera);
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

void LightDataModule::StoreLightCameras(BufferManager* bm, UploadTask* task, ObjectManager* om, SceneData* scene) {
    om->ForEach<Positions, SpotLightComponent, ShadowCasterComponent>(scene,
        [&](SoAElement<Positions> pos_el, SpotLightComponent& light, ShadowCasterComponent) {
            Positions& P = pos_el.container();
            size_t i = pos_el.i();
            //SDL_Log("StoreLightCameras: entity %u", e);
			StoreSpotLightCamera(bm, task, P, i, light.light_data);
	});
    om->ForEach<Positions, SphereLightComponent, ShadowCasterComponent>(scene,
        [&](SoAElement<Positions> pos_el, SphereLightComponent& light, ShadowCasterComponent) {
            Positions& P = pos_el.container();
            size_t i = pos_el.i();
            StoreSphereLightCameras(bm, task, P, i, light.light_data);
	});
}

uint32_t LightDataModule::AskNumLightCameras(ObjectManager* om, SceneData* scene)
{
    uint32_t num_light_cameras = 0;

    om->ForEachArchetype<Positions, SpotLightComponent>(
        scene,
        [&](ComponentArray<Positions, void>* posArr,
            ComponentArray<SpotLightComponent, void>*)
    {
        num_light_cameras += safe_u32(posArr->size());
    }
    );
    om->ForEachArchetype<Positions, SphereLightComponent>(
        scene,
        [&](ComponentArray<Positions, void>* posArr,
            ComponentArray<SphereLightComponent, void>*)
    {
        num_light_cameras += safe_u32(posArr->size()) * 6;
    }
    );

    return num_light_cameras;
}
