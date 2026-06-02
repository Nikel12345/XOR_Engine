#include "PCH.h"
#include "BatchBuilder.h"
#include "RenderCommandData.h"
#include "ObjectManager.h"
#include "PipeManager.h"
#include "RenderManager.h"
#include "ShaderManager.h"
#include "ModelData.h"
#include "TextureData.h"


using namespace BatchKeys;

ModelBatchKey HashModelBatchKey(SubMeshData* submash) {
    if (!submash) {
        SDL_Log("HashModelBatchKey: model is nullptr!");
        return 0xFFFFFFFFFFFFFFFFull;
    }
    ModelBatchKey key = 0;
    key ^= reinterpret_cast<ModelBatchKey>(submash);

    key ^= key >> 33;
    key *= 0xff51afd7ed558ccd;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53;
    key ^= key >> 33;

    return key;
}

TextureBatchKey HashTextureBatchKey(const Material* mat) {
    if (!mat) {
        SDL_Log("HashTextureBatchKey: material is nullptr!");
        return 0xFFFFFFFFFFFFFFFFull;
    }
    TextureBatchKey key = 0;
    //if (mat->albedo) {
    //    key ^= reinterpret_cast<TextureBatchKey>(mat->albedo->texture_data);
    //}
    //if (mat->normal_texture) {
    //    key ^= reinterpret_cast<TextureBatchKey>(mat->normal_texture->texture_data);
    //}
    for (const auto& [slot, tex] : mat->textures) {
        if (tex) {
            key ^= reinterpret_cast<TextureBatchKey>(tex->texture_data) * (2654435761ull + static_cast<uint64_t>(slot));
        }
    }
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccd;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53;
    key ^= key >> 33;
    return key;

}

AtlasBatchKey HashAtlasBatchKey(const Material* mat) {
    if (!mat) {
        SDL_Log("HashAtlasBatchKey: material is nullptr!");
        return 0xFFFFFFFFFFFFFFFFull;
    }
    AtlasBatchKey key = 0;
    //if (mat->albedo) {
    //    key ^= reinterpret_cast<AtlasBatchKey>(mat->albedo->atlas);
    //}
    //if (mat->normal_texture) {
    //    key ^= reinterpret_cast<AtlasBatchKey>(mat->normal_texture->atlas) * 2654435761ull;
    //}
    for (const auto& [slot, tex] : mat->textures) {
        if (tex && tex->atlas) {
            key ^= reinterpret_cast<AtlasBatchKey>(tex->atlas) * (2654435761ull + static_cast<uint64_t>(slot));
        }
    }
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccd;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53;
    key ^= key >> 33;

    return key;
}

ShaderBatchKey HashShaderBatchKey(ShaderProgram* sp) {
    if (!sp) {
        SDL_Log("HashShaderBatchKey: ShaderProgram is nullptr!");
        return 0xFFFFFFFFFFFFFFFFull;
    }
    ShaderBatchKey key = 0;
    key ^= reinterpret_cast<ShaderBatchKey>(sp);
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccd;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53;
    key ^= key >> 33;
    return key;
}

ShaderBatchKey HashShaderBatchKey(ComputeShaderProgram* sp) {
    if (!sp) {
        SDL_Log("HashShaderBatchKey: ShaderProgram is nullptr!");
        return 0xFFFFFFFFFFFFFFFFull;
    }
    ShaderBatchKey key = 0;
    key ^= reinterpret_cast<ShaderBatchKey>(sp);
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccd;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53;
    key ^= key >> 33;
    return key;
}

BatchBuilder::BatchBuilder()
{
}

inline void BatchProcessing(Entity e, PipeManager* pm, const MaterialComponent& matComp, const ModelComponent& modelComp, uint32_t entity_global_id) {

    for (SubMeshData& submesh : modelComp.model->submeshes)
    {
        if (matComp.materials.size() != modelComp.model->submeshes.size()) {
            SDL_Log("BulidBatches:: Submash and material sizes missmatch");
        }
        Material* material = matComp.materials[submesh.material_index];

        for (ShaderProgram* sp : material->shader_programs)
        {
            if (!sp) {
                SDL_Log("BatchBuilder::Using non existing shader program in material");
                continue;
            }
            RenderPassStep* rp = sp->associated_render_pass;
            if (!rp) continue;

            auto& shader_map = rp->shader_batches;
            auto sp_key = HashShaderBatchKey(sp);
            auto it = shader_map.find(sp_key);
            if (it == shader_map.end())
            {
                ShaderBatchData new_batch{};
                new_batch.push_func = sp->push_func;
                new_batch.pipeline = pm->GetGraphicPipeline(sp);
                new_batch.vertexStorageBuffers = sp->vertex_shader_buffers;
                new_batch.fragmentStorageBuffers = sp->fragment_shader_buffers;
                shader_map[sp_key] = std::move(new_batch);
            }

            ShaderBatchData& sb = shader_map[sp_key];

            AtlasBatchKey atlas_key = sp->required_slots.empty() ? 0 : HashAtlasBatchKey(material);

            auto& atlas_map = sb.atlases_batches;
            auto atlas_it = atlas_map.find(atlas_key);
            if (atlas_it == atlas_map.end())
            {
                AtlasBatchData new_tex{};

                for (const auto& role : sp->required_slots) {
                    auto it = material->textures.find(role);
                    if (it == material->textures.end() || !it->second || !it->second->atlas) {
                        SDL_Log("BuildBatches:: Material is missing required atlas for shader slot");
                        assert(false && "Material is missing required texture for shader slot!");
                        continue;
                    }
                    new_tex.texture_binding.push_back(it->second->atlas->texture_binding);

                }

                atlas_map[atlas_key] = std::move(new_tex);
            }

            AtlasBatchData& atlas_batch = atlas_map[atlas_key];

            TextureBatchKey tex_key = sp->required_slots.empty() ? 0 : HashTextureBatchKey(material);

            auto& tex_map = atlas_batch.texture_batches;
            auto texb_it = tex_map.find(tex_key);
            if (texb_it == tex_map.end()) {
                TextureBatchData new_texb{};
                new_texb.texture_uvl.reserve(material->textures.size());
                for (const auto& role : sp->required_slots) {
                    auto it = material->textures.find(role);
                    if (it == material->textures.end() || !it->second || !it->second->texture_data) {
                        SDL_Log("BuildBatches:: Material is missing required texture_data for shader slot");
                        assert(false && "Material is missing required texture for shader slot!");
                        continue;
                    }
                    new_texb.texture_uvl.push_back(it->second->texture_data);
                }

                tex_map[tex_key] = std::move(new_texb);
            }

            TextureBatchData& tex_batch = tex_map[tex_key];
            ModelBatchKey model_key = HashModelBatchKey(&submesh);

            auto& model_map = tex_batch.model_batches;
            auto model_it = model_map.find(model_key);
            if (model_it == model_map.end())
            {
                ModelBatchData new_model{};
                new_model.submesh = &submesh;
                new_model.instanceCount = 0;
                new_model.pib_sub_buffer.reserve(16);  // ��������� ������
                model_map[model_key] = std::move(new_model);
            }

            ModelBatchData& model_batch = model_map[model_key];

            model_batch.instanceCount++;
            model_batch.pib_sub_buffer.push_back(entity_global_id);

        }
    }

}

bool BatchBuilder::BuildRenderBatches(PipeManager* pm, PassManager* pass_manager, ObjectManager* om, SceneData* scene)
{
    if (!dirty_batches) {
        return false;
    }

    if (!scene) {
        SDL_Log("BuildBatches called with null scene!");
        return false;
    }

    for (RenderPassStep* rp : pass_manager->GetOrderedRenderPasses()) {
        rp->shader_batches.clear();
    }

    uint32_t entity_global_id = 0;

    om->ForEach<MaterialComponent, ModelComponent, Positions>(
        scene,
        [&](Entity e, const MaterialComponent& matComp, const ModelComponent& modelComp, const Positions&)
    {
        BatchProcessing(e, pm, matComp, modelComp, entity_global_id);
        entity_global_id++;

    }
    );

    FinilizeRenderBatches(pass_manager);
    dirty_batches = false;
    need_PIB_upload = true;
    return true;
}

void BatchBuilder::RecalculateBatches(PipeManager* pm, PassManager* pass_manager, std::vector<Entity>& entites)
{
}

void BatchBuilder::FinilizeRenderBatches(PassManager* pass_manager)
{
    auto UnpackUnorm16x2 = [](uint32_t packed, float& x, float& y) {
        uint16_t lx = static_cast<uint16_t>(packed & 0xFFFF);
        uint16_t ly = static_cast<uint16_t>(packed >> 16);
        x = lx / 65535.0f;
        y = ly / 65535.0f;
        };

    int pass_idx = 0;
    uint32_t offset = 0;
    uint32_t command_index = 0;

    for (RenderPassStep* rp : pass_manager->GetOrderedRenderPasses())
    {
        SDL_Log("[RenderPassStep %d]", pass_idx++);

        for (auto& [sp_key, sb] : rp->shader_batches)
        {
            SDL_Log("  [ShaderBatch key=%llu]", sp_key);

            for (auto& [atlas_key, ab] : sb.atlases_batches)
            {
                SDL_Log("    [AtlasBatch key=%llu, bindings=%zu]", atlas_key, ab.texture_binding.size());

                for (auto& [tex_key, texb] : ab.texture_batches)
                {
                    SDL_Log("      [TextureBatch key=%llu, uvl_count=%zu]", tex_key, texb.texture_uvl.size());

                    for (size_t i = 0; i < texb.texture_uvl.size(); i++) {
                        if (texb.texture_uvl[i]) {
                            float ox, oy, sx, sy;
                            UnpackUnorm16x2(texb.texture_uvl[i]->uv_packed_offset, ox, oy);
                            UnpackUnorm16x2(texb.texture_uvl[i]->uv_packed_scale, sx, sy);
                            SDL_Log("        [TextureData %zu] offset=(%.4f, %.4f) scale=(%.4f, %.4f) layer=%u",
                                i, ox, oy, sx, sy, texb.texture_uvl[i]->layer);
                        }
                        else {
                            SDL_Log("        [TextureData %zu] nullptr", i);
                        }
                    }
                    texb.indirect_command_index = command_index;

                    for (auto& [model_key, mb] : texb.model_batches)
                    {
                        mb.firstInstance = offset;
                        offset += mb.instanceCount;

                        if (mb.submesh) {
                            SDL_Log("           [ModelBatch key=%llu] mat_idx=%u vOffset=%u iOffset=%u iCount=%u | instances=%u firstInstance=%u pib=[",
                                model_key,
                                mb.submesh->material_index,
                                mb.submesh->vertexOffset,
                                mb.submesh->indexOffset,
                                mb.submesh->indexCount,
                                mb.instanceCount,
                                mb.firstInstance);
                            std::string pib_str;
                            for (uint32_t id : mb.pib_sub_buffer)
                                pib_str += std::to_string(id) + " ";
                            SDL_Log("          pib: %s]", pib_str.c_str());
                        }
                        command_index++;
                    }
                }
            }
        }
    }
    total_commands = command_index;
}

//void BatchBuilder::FinilizeRenderBatches(PassManager* pass_manager)
//{
//    auto UnpackUnorm16x2 = [](uint32_t packed, float& x, float& y) {
//        uint16_t lx = static_cast<uint16_t>(packed & 0xFFFF);
//        uint16_t ly = static_cast<uint16_t>(packed >> 16);
//        x = lx / 65535.0f;
//        y = ly / 65535.0f;
//    };
//
//    uint32_t offset = 0;
//    uint32_t command_index = 0;
//
//    for (RenderPassStep* rp : pass_manager->GetOrderedRenderPasses()) {
//        for (auto& [sp_key, sb] : rp->shader_batches) {
//            for (auto& [atlas_key, ab] : sb.atlases_batches) {
//                for (auto& [tex_key, texb] : ab.texture_batches) {
//                    texb.indirect_command_index = command_index;
//                    for (auto& [model_key, mb] : texb.model_batches) {
//                        mb.firstInstance = offset;
//                        offset += mb.instanceCount;
//                        command_index++;
//                    }
//                }
//            }
//        }
//    }
//    total_commands = command_index;
//}

void BatchBuilder::BuildComputeBatches(PipeManager* pm, ShaderManager* sm) {
    if (!sm || !sm->IsDirtyComputePipelines()) return;

    auto& compute_programs = sm->GetComputeShaderPrograms();
    for (auto& sp : compute_programs) {
        SDL_GPUComputePipeline* pipe = pm->GetComputePipeline(sp.get());
        if (!pipe) continue;

        ComputePassStep* cmp = sp->associated_compute_pass;
        if (!cmp) continue;

        ComputeShaderBatchData new_batch{};
        new_batch.pipeline = pipe;
        new_batch.rw_storage_buffers = sp->rw_storage_buffers;
        new_batch.ro_storage_buffers = sp->ro_storage_buffers;

        new_batch.rw_storage_textures.reserve(sp->rw_storage_textures.size());
        for (const auto& d : sp->rw_storage_textures) {
            new_batch.rw_storage_textures.emplace_back(
                d.texture_atlas->texture_binding.texture, d.mip_level, d.layer, false);
        }
        new_batch.ro_storage_textures.reserve(sp->ro_storage_textures.size());
        for (const auto& a : sp->ro_storage_textures) {
            new_batch.ro_storage_textures.push_back(a->texture_binding.texture);
        }
        new_batch.texture_binding.reserve(sp->texture_samplers.size());
        for (const auto& a : sp->texture_samplers) {
            new_batch.texture_binding.push_back(a->texture_binding);
        }
        new_batch.push_func = sp->push_func;
        new_batch.dispatch_func = sp->dispatch_func;

        new_batch.threadcount_x = sp->cs.threadcount_x;
        new_batch.threadcount_y = sp->cs.threadcount_y;
        new_batch.threadcount_z = sp->cs.threadcount_z;

        new_batch.debug_name = sp->debug_name;

        cmp->shader_batches.push_back(std::move(new_batch));
    }
    sm->SetDirtyComputePipelines(false);
}



uint32_t BatchBuilder::AskNumCommands()
{
    return total_commands;
}
