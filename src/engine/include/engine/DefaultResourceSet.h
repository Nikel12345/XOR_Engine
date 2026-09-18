#pragma once

class EngineContext;
class BufferManager;
class TextureManager;

namespace DefaultResourceSet
{
    void SetDefaultResources(EngineContext* ctx);
    void CreateDefaultBuffers(BufferManager* bm);
    void CreateDefaultTextureResources(TextureManager* tm);
}
