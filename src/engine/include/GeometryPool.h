#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "Aliases.h"
#include "ShaderTypes.h"

// Пул геометрии: раскладка вершины и её разбивка на стримы — docs/gpu/geometry/pools.md.
class GeometryPool
{
public:
    struct StreamDesc {
        // Два разных отсчёта: offset у атрибута — внутри вершины СТРИМА, src_offset — внутри
        // вершины РАСКЛАДКИ.
        std::vector<ShaderBase::VertexAttr> attrs;
        uint32_t stride = 0;
        uint32_t src_offset = 0;
    };

    struct Stream {
        // Строкой владеет пул, и ключ реестра BufferManager указывает в неё же.
        BufferDataName                  buffer_name = nullptr;
        const ShaderBase::VertexFormat* format = nullptr;
        uint32_t                        src_offset = 0;
    };

    GeometryPool(const std::string& name, uint32_t vertex_size, const std::vector<StreamDesc>& descs);

    uint32_t                   VertexSize()  const { return vertex_size_; }
    BufferDataName             IndexBuffer() const { return index_buffer_; }
    const std::vector<Stream>& Streams()     const { return streams_; }
    const std::string&         Name()        const { return debug_name_; }

    // Стримы в каноническом порядке слотов, без дублей: NORMAL и TANGENT сходятся в один стрим.
    std::vector<const Stream*> StreamsForSemantics(const std::vector<ShaderBase::VertexSemantic>& pull) const;
    std::vector<ShaderBase::VertexSemantic> AvailableSemantics() const;

    bool     HasFloat3Position() const { return position_offset_ != kNoPosition; }
    // Байтовое смещение позиции внутри вершины РАСКЛАДКИ. Валидно при HasFloat3Position.
    uint32_t PositionOffset()    const { return position_offset_; }

private:
    static constexpr uint32_t kNoPosition = ~0u;

    std::string debug_name_;
    // На эти два контейнера указывают Stream и ключи реестра буферов: заполняются один раз в
    // конструкторе, иначе реаллокация повесила бы указатели.
    std::vector<std::string>              owned_names_;
    std::vector<ShaderBase::VertexFormat> formats_;
    std::vector<Stream>                   streams_;
    std::string                           index_name_;

    uint32_t       vertex_size_ = 0;
    BufferDataName index_buffer_ = nullptr;
    uint32_t       position_offset_ = kNoPosition;
};
