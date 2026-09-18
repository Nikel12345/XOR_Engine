#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "Utils.h"

// Настройки графики: ДОМЕН, в котором движок считает кадр, и только он. Сколько всего пикселей
// движок согласен посчитать и с какой частотой сэмплируется сцена — не зависит от того, какие проходы
// собраны, поэтому набор полей здесь закрыт. Доля КОНКРЕТНОГО эффекта сюда не входит: она живёт в
// состоянии его прохода (BloomState/AOState), потому что набор проходов открыт, а эта структура нет.
// Намеренно НЕ тип ParamsSpecRegistry (как параметры материала или состояние прохода) — на четыре
// поля схема, генерируемый инспектор и round-trip по именам не окупаются.
//
// ВЛАДЕНИЕ И ПОТОК. Объект живёт на куче у Engine, указатель стабилен всю его жизнь: читатели
// держат указатель, писатель правит поля на месте. ПИШЕТ его только render-поток (панель редактора
// исполняется внутри Engine::RenderFunc), ЧИТАЕТ ещё и sim-поток: Engine::GetWidth/GetHeight выводят
// внутреннее разрешение прямо отсюда. Синхронизации нет намеренно — это набор скаляров, и худшее,
// что даст рваное чтение, это неверный размер на один кадр: инструкции ресайза считают заново каждый
// кадр и сойдутся сами.
struct GraphicsConfig {
    // База внутреннего разрешения — размер scene_hdr и всего, что живёт в его разрешении
    // (emission/ambient/depth). 0 = следовать за размером назначения (по умолчанию render == окно).
    uint32_t render_w = 0;
    uint32_t render_h = 0;

    // Частота сэмплирования сцены относительно вывода, поверх базы выше. Это и есть сглаживание:
    // 2.0 = SSAA 2x (рендерим вчетверо больше пикселей, present-блит усредняет их вниз), меньше
    // единицы = масштабирование ради fps. Эффектов НЕ касается — тем лишние сэмплы не нужны.
    // Множителем, а не абсолютом, чтобы настройка переживала ресайз окна: с абсолютным render_w
    // растянутое окно молча превратило бы «сглаживание 2x» в 1.5x, а потом и в даунскейл.
    float render_scale = 1.0f;

    // Общий множитель на ВСЕ экранные таргеты: сброс качества целиком (слабое железо, окно-превью).
    // От render_scale отличается тем, что задевает и эффекты: тот меняет частоту сэмплирования
    // сцены, этот — сколько всего пикселей движок согласен посчитать.
    float global_scale = 1.0f;
};

// Округление ВВЕРХ, а не вниз: при нечётной стороне «вниз» потеряло бы крайний столбец кадра.
// Ноль, отрицательное и NaN сводятся к 1 — таргета нулевого размера не бывает.
inline uint32_t GfxScaleDim(uint32_t v, float scale)
{
    const float f = std::ceil(static_cast<float>(v) * scale);
    if (!(f >= 1.0f)) return 1;
    return safe_f_u32(f);
}

// База внутреннего домена: либо пин из конфига, либо размер назначения. Ещё без масштабов.
inline uint32_t GfxRenderBaseW(const GraphicsConfig& c, uint32_t out_w) { return c.render_w ? c.render_w : out_w; }
inline uint32_t GfxRenderBaseH(const GraphicsConfig& c, uint32_t out_h) { return c.render_h ? c.render_h : out_h; }

// База эффектного домена: min(render, назначение). Минимум обязателен, потому что render_scale
// работает в обе стороны: выше render информации нет (её никто не посчитал), выше назначения её
// некуда показать. render здесь взят С render_scale, но БЕЗ global_scale — тот применяется в самом
// конце и одинаково ко всем таргетам, включая эффектные, поэтому вносить его в минимум значило бы
// учесть дважды.
inline uint32_t GfxEffectBaseW(const GraphicsConfig& c, uint32_t out_w)
{ return std::min(GfxScaleDim(GfxRenderBaseW(c, out_w), c.render_scale), out_w); }
inline uint32_t GfxEffectBaseH(const GraphicsConfig& c, uint32_t out_h)
{ return std::min(GfxScaleDim(GfxRenderBaseH(c, out_h), c.render_scale), out_h); }

// Конечные размеры таргетов. Внутреннее разрешение, которое движок сообщает игре (Engine::GetWidth),
// выводится ЭТОЙ же функцией, что и размер scene_hdr: разойдись они — аспект камеры соврёт, а узнать
// это можно будет только по картинке.
inline void GfxRenderTarget(const GraphicsConfig& c, uint32_t out_w, uint32_t out_h, uint32_t& w, uint32_t& h)
{
    w = GfxScaleDim(GfxRenderBaseW(c, out_w), c.render_scale * c.global_scale);
    h = GfxScaleDim(GfxRenderBaseH(c, out_h), c.render_scale * c.global_scale);
}
inline void GfxEffectTarget(const GraphicsConfig& c, uint32_t out_w, uint32_t out_h, float scale, uint32_t& w, uint32_t& h)
{
    w = GfxScaleDim(GfxEffectBaseW(c, out_w), scale * c.global_scale);
    h = GfxScaleDim(GfxEffectBaseH(c, out_h), scale * c.global_scale);
}
