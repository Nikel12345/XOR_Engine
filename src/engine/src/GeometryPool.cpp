#include "PCH.h"
#include "GeometryPool.h"

using namespace ShaderBase;

// Суффикс имени буфера — по ведущей семантике стрима. Уникальность имени этим и обеспечена:
// имя пула уникально (ключ реестра ModelManager), а семантика не повторяется между стримами
// (проверяется ниже). Ведущее подчёркивание — служебный ассет для фильтра редактора
// (ui::IsInternalName): стрим-буферы в дропдаунах пользователю не нужны.
static const char* SemSuffix(VertexSemantic s)
{
    switch (s) {
    case POSITION: return "Pos";
    case UV:       return "UV";
    case NORMAL:   return "Norm";
    case TANGENT:  return "Tan";
    }
    return "Attr";
}

GeometryPool::GeometryPool(const std::string& name, uint32_t vertex_size, const std::vector<StreamDesc>& descs)
{
    debug_name_ = name;
    vertex_size_ = vertex_size;

    if (descs.empty() || vertex_size == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "GeometryPool '%s': empty stream list or zero vertex size.", name.c_str());
        assert(false && "GeometryPool: пустая раскладка");
        return;
    }

    // Имена и форматы заполняются ПОЛНОСТЬЮ до сборки streams_: те держат на них указатели, и
    // реаллокация после этого их повесила бы (см. owned_names_/formats_ в заголовке).
    owned_names_.reserve(descs.size());
    formats_.reserve(descs.size());
    streams_.reserve(descs.size());
    index_name_ = "_" + name + "_Index";
    index_buffer_ = index_name_.c_str();

    // Отбракованные описания пропускаются, поэтому src_offset'ы принятых копим отдельно —
    // индексы descs и formats_ после первого же skip разъезжаются.
    std::vector<uint32_t> accepted_src;
    accepted_src.reserve(descs.size());

    for (const StreamDesc& d : descs) {
        if (d.attrs.empty() || d.stride == 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "GeometryPool '%s': stream without attributes or with zero stride.", name.c_str());
            assert(false && "GeometryPool: пустой стрим");
            continue;
        }
        // Стрим обязан помещаться в вершину раскладки: иначе расщепление при заливке читает за её
        // границей (UploadModelVertexStream шагает по vertex_size, выдирая [src_offset, +stride)).
        if (d.src_offset + d.stride > vertex_size) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "GeometryPool '%s': stream [%u, +%u) does not fit into a %u-byte layout vertex.",
                name.c_str(), d.src_offset, d.stride, vertex_size);
            assert(false && "GeometryPool: стрим выходит за вершину раскладки");
            continue;
        }
        for (const VertexAttr& a : d.attrs)
            if (a.offset >= d.stride) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "GeometryPool '%s': attribute offset %u is outside its own %u-byte stream vertex.",
                    name.c_str(), a.offset, d.stride);
                assert(false && "GeometryPool: атрибут вне своего стрима");
            }

        owned_names_.push_back("_" + name + "_" + SemSuffix(d.attrs.front().semantic));
        VertexFormat fmt;
        fmt.attrs = d.attrs;
        fmt.stride = d.stride;
        formats_.push_back(std::move(fmt));
        accepted_src.push_back(d.src_offset);
    }

    for (size_t i = 0; i < formats_.size(); ++i) {
        Stream s{};
        s.buffer_name = owned_names_[i].c_str();
        s.format = &formats_[i];
        s.src_offset = accepted_src[i];
        streams_.push_back(s);
    }

    for (size_t i = 0; i < streams_.size(); ++i)
        for (size_t j = i + 1; j < streams_.size(); ++j) {
            // Семантика в двух стримах сделала бы StreamsForSemantics неоднозначной (а на ней держится
            // резолв pull → слоты) и сломала бы уникальность сгенерированного имени.
            for (const VertexAttr& a : streams_[i].format->attrs)
                if (streams_[j].format->Find(a.semantic)) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                        "GeometryPool '%s': semantic %u declared in two streams.", name.c_str(), (unsigned)a.semantic);
                    assert(false && "GeometryPool: семантика в двух стримах");
                }
            // Перекрытие в вершине раскладки означало бы, что один байт кормит два буфера.
            const uint32_t a0 = streams_[i].src_offset, a1 = a0 + streams_[i].format->stride;
            const uint32_t b0 = streams_[j].src_offset, b1 = b0 + streams_[j].format->stride;
            if (a1 > b0 && b1 > a0) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "GeometryPool '%s': streams [%u,%u) and [%u,%u) overlap in the layout vertex.",
                    name.c_str(), a0, a1, b0, b1);
                assert(false && "GeometryPool: стримы перекрываются");
            }
        }

    // Позиция ВЫВОДИТСЯ из таблицы стримов, а не хранится отдельным входом — иначе появился бы
    // второй источник правды, разъезжающийся с раскладкой. POSITION не во FLOAT3 (упакованная) —
    // законная раскладка, просто пивот и сканирование границ для неё недоступны.
    for (const Stream& s : streams_)
        if (const VertexAttr* a = s.format->Find(POSITION)) {
            if (a->format == SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3)
                position_offset_ = s.src_offset + a->offset;
            break;
        }
}

std::vector<const GeometryPool::Stream*> GeometryPool::StreamsForSemantics(const std::vector<VertexSemantic>& pull) const
{
    std::vector<const Stream*> out;
    out.reserve(streams_.size());
    for (const Stream& s : streams_)
        for (VertexSemantic sem : pull)
            if (s.format->Find(sem)) { out.push_back(&s); break; }
    return out;
}

std::vector<VertexSemantic> GeometryPool::AvailableSemantics() const
{
    std::vector<VertexSemantic> out;
    for (const Stream& s : streams_)
        for (const VertexAttr& a : s.format->attrs)
            out.push_back(a.semantic);   // дублей быть не может — проверено в конструкторе
    return out;
}
