#include "PCH.h"
#include "TransformDataModule.h"
#include "BufferManager.h"
#include "ObjectManager.h"

TransformDataModule::TransformDataModule()
{
}

void TransformDataModule::UpdateLocalTransforms(ObjectManager* om, SceneData* scene)
{
    om->ForEach<Positions, ParentComponent, LocalOffsets>(scene,
        [&](SoAElement<Positions> pos_el, ParentComponent& parentComp, SoAElement<LocalOffsets> local_el)
    {
        Positions& pos = pos_el.container();
        LocalOffsets& local = local_el.container();
        size_t i = pos_el.i();

        Entity parent = parentComp.parent;

        Archetype* parentArch = scene->entity_to_archetype[parent];
        if (!parentArch) {
            assert(false && "No parent component");
            return;
        }

        auto* parentPosArr = parentArch->get_array<Positions>();
        if (!parentPosArr)
            return;

        size_t pIndex = scene->entity_to_index[parent];
        Positions& parentPos = parentPosArr->data;

        float px = parentPos.w[pIndex];
        float py = parentPos.d[pIndex];
        float pz = parentPos.h[pIndex];

        pos.w[i] = px + local.ox[i];
        pos.d[i] = py + local.oy[i];
        pos.h[i] = pz + local.oz[i];
    });
}

uint32_t TransformDataModule::CalculateTransformSize(ObjectManager* om, SceneData* scene)
{
    if (!om->CheckNewObjects()) {
        om->NewObjectsCommit();
        return total_size;
    }

    total_size = 0;

    om->ForEachArchetype<Positions, ModelComponent, MaterialComponent>(
        scene,
        [&](ComponentArray<Positions, void>* posArr,
            ComponentArray<ModelComponent, void>*,
            ComponentArray<MaterialComponent, void>*)
    {
        total_size += safe_u32(posArr->size()) * sizeof(PositionProxy16);
    }
    );

    return total_size;
}

void TransformDataModule::StoreTransforms(BufferManager* bm, UploadTask* task, ObjectManager* om, SceneData* scene) {
	this->UpdateLocalTransforms(om, scene);
    om->ForEach<MaterialComponent, ModelComponent, Positions>(scene, [&](const MaterialComponent& material, const ModelComponent& mod, SoAElement<Positions> pos_el) {
        Positions& pos = pos_el.container();
        size_t i = pos_el.i();
        float mat[16]; 

        mat[0] = pos.x[i]; 
        mat[1] = pos.y[i]; 
        mat[2] = pos.z[i];  
        mat[3] = pos.i[i];  

        mat[4] = pos.a[i];  
        mat[5] = pos.b[i];  
        mat[6] = pos.c[i];   
        mat[7] = pos.j[i];  

        mat[8] = pos.e[i];  
        mat[9] = pos.f[i];  
        mat[10] = pos.g[i];  
        mat[11] = pos.k[i];  

        mat[12] = pos.w[i];  
        mat[13] = pos.d[i]; 
        mat[14] = pos.h[i]; 
        mat[15] = pos.l[i]; 

        bm->UploadToTransferBuffer(task, sizeof(mat), mat);
        
    });
}

uint32_t TransformDataModule::AskNumTransform(ObjectManager* om, SceneData* scene)
{

    uint32_t num_transform = 0;

    om->ForEachArchetype<Positions, ModelComponent, MaterialComponent>(
        scene,
        [&](ComponentArray<Positions, void>* posArr,
            ComponentArray<ModelComponent, void>*,
            ComponentArray<MaterialComponent, void>*)
    {
        num_transform += safe_u32(posArr->size());
    }
    );

    return num_transform;
}

void TransformDataModule::ReadBackCullingCountReader(BufferManager* bm, ReadBackTask* task)
{
    size_ptr = bm->ReadFromTransferBuffer(task, sizeof(uint32_t));
    std::cout << *reinterpret_cast<const uint32_t*>(size_ptr.data()) << "\n";
}

uint32_t TransformDataModule::CalculateOutTransformSize()
{
    if (size_ptr.size() < sizeof(uint32_t))
    {
        SDL_Log("Not enough data in size_ptr for uint32_t");
        return 0;
    }

    return *reinterpret_cast<const uint32_t*>(size_ptr.data()) * sizeof(glm::mat4);
}


