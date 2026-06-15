#include "PCH.h"
#include "TransformDataModule.h"
#include "BufferManager.h"
#include "ObjectManager.h"
#include <unordered_set>

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


//void TransformDataModule::StoreTransforms(BufferManager* bufferManager, ObjectManager* objectManager, SceneData* scene)
//{
//    UpdateLocalTransforms(objectManager, scene);
//
//    auto t0 = std::chrono::high_resolution_clock::now();
//    bufferManager->BeginWrite(buffer_data);
//
//    objectManager->ForEach<Positions>(scene, [&](const Positions& pos) {
//        Uint32 arr_size = static_cast<Uint32>(pos.size() * sizeof(float));
//
//        const f_restrict_pointer soa_ptrs[16] = {
//            pos.x.data(), pos.y.data(), pos.z.data(), pos.w.data(),
//            pos.a.data(), pos.b.data(), pos.c.data(), pos.d.data(),
//            pos.e.data(), pos.f.data(), pos.g.data(), pos.h.data(),
//            pos.i.data(), pos.j.data(), pos.k.data(), pos.l.data()
//        };
//        for (size_t i = 0; i < 16; ++i) {
//            bufferManager->AppendWrite(soa_ptrs[i], arr_size);
//            //bm->AppendWrite(ptr, arr_size);
//            //bm->uploadToGPUBuffer(buffer_data, soa_ptrs[i], arr_size);
//        }
//
//		//std::cout << "Stored " << pos.size() << " transforms.\n";
//    });
//
//    bufferManager->EndWrite();
//    auto t1 = std::chrono::high_resolution_clock::now();
//    double seconds = std::chrono::duration<double>(t1 - t0).count();
//    std::cout << "StoreTransforms taken " << seconds << " s" << "\n";
//}

//void TransformDataModule::StoreTransforms(BufferManager* bufferManager, UploadTask* task, ObjectManager* objectManager, SceneData* scene)
//{
//    UpdateLocalTransforms(objectManager, scene);
//
//    chunks.clear();
//
//    std::unordered_set<const Positions*> seen;
//    seen.reserve(256);
//
//    objectManager->ForEach<Positions, ModelComponent>(scene,
//        [&](const Positions& pos, const ModelComponent& /*mod*/)
//    {
//        // Если это SoA-контейнер чанка: size() должно отражать кол-во элементов в чанке
//        if (pos.size() == 0) return;
//
//        const Positions* p = &pos;
//        if (seen.insert(p).second) { // добавили впервые
//            chunks.push_back(p);
//        }
//    });
//
//    if (chunks.empty()) return;
//
//    auto write_column = [&](auto Positions::* memberPtr) {
//        for (const Positions* p : chunks) {
//            const auto& vec = p->*memberPtr;
//            const Uint32 bytes = static_cast<Uint32>(vec.size() * sizeof(float));
//            bufferManager->UploadToTransferBuffer(task, bytes, vec.data());
//        }
//    };
//
//    write_column(&Positions::x);
//    write_column(&Positions::y);
//    write_column(&Positions::z);
//    write_column(&Positions::w);
//
//    write_column(&Positions::a);
//    write_column(&Positions::b);
//    write_column(&Positions::c);
//    write_column(&Positions::d);
//
//    write_column(&Positions::e);
//    write_column(&Positions::f);
//    write_column(&Positions::g);
//    write_column(&Positions::h);
//
//    write_column(&Positions::i);
//    write_column(&Positions::j);
//    write_column(&Positions::k);
//    write_column(&Positions::l);
//}

