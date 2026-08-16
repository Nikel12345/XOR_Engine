#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "Aliases.h"
#include "ShaderTypes.h"

// Пул геометрии = РАСКЛАДКА ВЕРШИН (CPU-формат загрузки) + её разбивка на СТРИМЫ. Стрим —
// отдельный GPU-буфер с плотной раскладкой; регионом общего буфера он быть не может, потому что
// регионы не растут независимо, а EnsureBufferCapacity/RESIZE_AND_COPY — пер-буферная машинерия.
//
// Все стримы пула растут В НОГУ: вершина №N существует в каждом. Это не выбор, а требование API —
// vertex_offset индирект-команды один на все привязанные вершинные буферы. Отсюда два следствия,
// на которых держится всё остальное: элементное пространство у пула ОДНО, и индексный буфер тоже
// один (first_index сквозной по пулу).
//
// Шейдер объявляет ПОТРЕБЛЯЕМОЕ подмножество стримов (теням хватает Pos — 12 байт/вершину вместо
// 44 интерлива); это бесплатно именно из-за общего элементного пространства.
//
// Создаётся ТОЛЬКО через EngineContext::CreateGeometryPool — тот регистрирует буферы стримов,
// индексный буфер и инструкции заливки одним вызовом. НЕ УДАЛЯЕТСЯ: его имена — ключи в реестре
// BufferManager, его стримы названы шейдерами, его диапазоны держат модели.
class GeometryPool
{
public:
    // Вход создания одного стрима. Транзитный тип: нигде не хранится, живёт только как аргумент.
    struct StreamDesc {
        // offset'ы атрибутов — ВНУТРИ вершины СТРИМА (Pos: xyz с нуля; NormTan: normal, за ним tangent).
        // Не путать с src_offset ниже: тот — в вершине РАСКЛАДКИ.
        std::vector<ShaderBase::VertexAttr> attrs;
        uint32_t stride = 0;       // размер вершины стрима (плотная раскладка на GPU)
        uint32_t src_offset = 0;   // где группа лежит в вершине РАСКЛАДКИ — для расщепления при заливке
    };

    struct Stream {
        // Имя генерирует пул (уникальность доказуема: имя пула уникально, первая семантика уникальна
        // внутри пула), и он же владеет строкой — ключ реестра BufferManager указывает СЮДА.
        BufferDataName                  buffer_name = nullptr;
        const ShaderBase::VertexFormat* format = nullptr;   // владеет пул (см. formats_)
        uint32_t                        src_offset = 0;
    };

    GeometryPool(const std::string& name, uint32_t vertex_size, const std::vector<StreamDesc>& descs);

    uint32_t                   VertexSize()  const { return vertex_size_; }
    BufferDataName             IndexBuffer() const { return index_buffer_; }
    const std::vector<Stream>& Streams()     const { return streams_; }
    // Копия ключа реестра пулов. Идентичность пула — сам указатель; строка нужна тем, кто ссылку
    // СЕРИАЛИЗУЕТ (ModelData::pool_name, VertexShaderData::pool_name) и логам.
    const std::string&         Name()        const { return debug_name_; }

    // Семантики (язык манифеста "pull" и формы редактора) → стримы в КАНОНИЧЕСКОМ порядке слотов,
    // без дублей: NORMAL и TANGENT сходятся в один стрим. Порядок самого pull не значим — порядок
    // слотов задаёт таблица стримов, и только она.
    std::vector<const Stream*> StreamsForSemantics(const std::vector<ShaderBase::VertexSemantic>& pull) const;
    // Объединение семантик по всем стримам — набор, который эта раскладка вообще может дать
    // (форма редактора предлагает только его, а не фиксированную четвёрку).
    std::vector<ShaderBase::VertexSemantic> AvailableSemantics() const;

    // Умеет ли пул пивот и подсчёт границ сканированием: POSITION есть И лежит как FLOAT3.
    // Раскладка без позиции (или с упакованной) законна — тогда границы обязаны прийти из данных,
    // а AnchorShift, кроме Keep, для неё ошибка.
    bool     HasFloat3Position() const { return position_offset_ != kNoPosition; }
    // Байтовое смещение позиции внутри вершины РАСКЛАДКИ. Валидно при HasFloat3Position.
    // Выводится из таблицы стримов, а не хранится отдельным входом — разъехаться не может.
    uint32_t PositionOffset()    const { return position_offset_; }

private:
    static constexpr uint32_t kNoPosition = ~0u;

    std::string debug_name_;
    // Строки имён и форматы: на них указывают Stream и ключи реестра буферов, поэтому оба
    // контейнера заполняются ОДИН раз в конструкторе и больше не растут — реаллокация повесила бы
    // указатели. Отсюда же запрет добавлять стрим в существующий пул.
    std::vector<std::string>              owned_names_;
    std::vector<ShaderBase::VertexFormat> formats_;
    std::vector<Stream>                   streams_;
    std::string                           index_name_;

    uint32_t       vertex_size_ = 0;
    BufferDataName index_buffer_ = nullptr;
    uint32_t       position_offset_ = kNoPosition;
};
