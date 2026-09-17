#pragma once
// Компоненты движка — только данные. Половина из них ходит парой «SoA-хранилище + прокси»
// (Positions и PositionProxy16, Velocities и VelocityProxy, ...): колонками лежит хранилище, а
// прокси — это одна строка полями, и только им компонент отдают в CreateEntity. Весь путь такой
// пары расписан в ComponentStorage.h.
#include "ComponentStorage.h"
#include "ResourceId.h"
#include <cmath>
#include <SDL3/SDL.h>

struct Accelerations : SoAProxyAddable<Accelerations> {
    using soa_tag = void;
    std::vector<float> x, y, z;
    size_t size() const { return x.size(); }
    auto columns() { return std::tie(x, y, z); }
};

struct AccelerationProxy {
    float x = 0, y = 0, z = 0;
    using related_soa = Accelerations;

    template<class SoA>
    void emplace_to(SoA& soa) const {
        soa.x.push_back(x);  soa.y.push_back(y);  soa.z.push_back(z);
    }
};

struct Velocities3 {
    float x = 0, y = 0, z = 0;
};
struct Velocities : SoAProxyAddable<Velocities> {
    using soa_tag = void;

    std::vector<float> x, y, z;
    size_t size() const { return x.size(); }
    auto columns() { return std::tie(x, y, z); }

    void MoveByAccelerations(const std::vector<float>& ax, const std::vector<float>& ay, const std::vector<float>& az);

};
struct VelocityProxy {
    float x = 0, y = 0, z = 0;
    using related_soa = Velocities;

    template<class SoA>
    void emplace_to(SoA& soa) const {
        soa.x.push_back(x);  soa.y.push_back(y);  soa.z.push_back(z);
    }
};

struct Positions : SoAProxyAddable<Positions> {
    using soa_tag = void;

    std::vector<float> x, y, z, w, a, b, c, d, e, f, g, h, i, j, k, l;
    size_t size() const { return x.size(); }
    auto columns() { return std::tie(x, y, z, w, a, b, c, d, e, f, g, h, i, j, k, l); }

    void MoveByVelocities(const std::vector<float>& vx, const std::vector<float>& vy, const std::vector<float>& vz);

};
struct PositionProxy16 {
    float x = 1, y = 0, z = 0, w = 0,
        a = 0, b = 1, c = 0, d = 0,
        e = 0, f = 0, g = 1, h = 0,
        i = 0, j = 0, k = 0, l = 1;
    using related_soa = Positions;

    template<class SoA>
    void emplace_to(SoA& soa) const {
        soa.x.push_back(x);  soa.y.push_back(y);  soa.z.push_back(z);  soa.w.push_back(w);
        soa.a.push_back(a);  soa.b.push_back(b);  soa.c.push_back(c);  soa.d.push_back(d);
        soa.e.push_back(e);  soa.f.push_back(f);  soa.g.push_back(g);  soa.h.push_back(h);
        soa.i.push_back(i);  soa.j.push_back(j);  soa.k.push_back(k);  soa.l.push_back(l);
    }

};

struct Positions16 {
    float x = 1, y = 0, z = 0, w = 0,
        a = 0, b = 1, c = 0, d = 0,
        e = 0, f = 0, g = 1, h = 0,
        i = 0, j = 0, k = 0, l = 1;
};

struct Parents : SoAProxyAddable<Parents> {
    using soa_tag = void;
    std::vector<Entity> parent;
    size_t size() const { return parent.size(); }
    auto columns() { return std::tie(parent); }
};

struct ParentProxy {
    Entity parent;
    using related_soa = Parents;

    template<class SoA>
    void emplace_to(SoA& soa) const {
        soa.parent.push_back(parent);
    }
};

// Иерархия: родитель сущности лежит здесь, а её матрица ОТНОСИТЕЛЬНО него — в LocalMatrices,
// разложенных column-major как есть (m12..m14 — трансляция). Каждый кадр
// TransformDataModule::UpdateLocalTransforms пишет Positions = матрица_родителя x эта.
// У Positions раскладка ДРУГАЯ: буквы идут по строкам, поэтому трансляция там в w, d, h.
struct ParentComponent {
    Entity parent;
};

struct EditorHiddenComponent {};

struct GeneratedComponent {};

struct LocalMatrices : SoAProxyAddable<LocalMatrices> {
    using soa_tag = void;
    std::vector<float> m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13, m14, m15;
    size_t size() const { return m0.size(); }
    auto columns() { return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13, m14, m15); }
};
struct LocalMatrixProxy16 {
    float m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    using related_soa = LocalMatrices;
    template<class SoA>
    void emplace_to(SoA& soa) const {
        soa.m0.push_back(m[0]);   soa.m1.push_back(m[1]);   soa.m2.push_back(m[2]);   soa.m3.push_back(m[3]);
        soa.m4.push_back(m[4]);   soa.m5.push_back(m[5]);   soa.m6.push_back(m[6]);   soa.m7.push_back(m[7]);
        soa.m8.push_back(m[8]);   soa.m9.push_back(m[9]);   soa.m10.push_back(m[10]); soa.m11.push_back(m[11]);
        soa.m12.push_back(m[12]); soa.m13.push_back(m[13]); soa.m14.push_back(m[14]); soa.m15.push_back(m[15]);
    }
};

struct LocalOffsets : SoAProxyAddable<LocalOffsets> {
    using soa_tag = void;
    std::vector<float> ox, oy, oz;
    size_t size() const { return ox.size(); }
    auto columns() { return std::tie(ox, oy, oz); }
};

struct LocalOffsetProxy {
    float ox, oy, oz;
    using related_soa = LocalOffsets;

    template<class SoA>
    void emplace_to(SoA& soa) const {
        soa.ox.push_back(ox);
        soa.oy.push_back(oy);
        soa.oz.push_back(oz);
    }
};

struct ModelComponent {
    ModelId model;
};

enum class TextureSlotRole;

struct MaterialRef {
    MaterialId                                        material;
    std::vector<std::pair<TextureSlotRole, uint32_t>> states;
};

struct MaterialComponent {
    std::vector<MaterialRef> materials;
};

struct TextureStateComponent {};

enum class LightTypes {
    SPOT,
    SPHERE,
    DIRECT
};

struct SpotLightComponent {
    struct SpotLightData {
        float source_radius = 0;
        float dir_x = 0, dir_y = 0, dir_z = 1;
        float source_angle = 0.3f;
        float r = 1, g = 1, b = 1;
        float power = 1;
        float attenuation = 1.0f;

        SpotLightData(
            float source_radius = 0,
            float dir_x = 0, float dir_y = 0, float dir_z = 0,
            float source_angle = 0.3f,
            float r = 1, float g = 1, float b = 1,
            float power = 1,
            float attenuation = 1.0f
        )
            : source_radius(source_radius),
            dir_x(dir_x), dir_y(dir_y), dir_z(dir_z),
            source_angle(source_angle),
            r(r), g(g), b(b),
            power(power),
            attenuation(attenuation) {
        }

        void ResolveDistance() {
            if (cached_attenuation != attenuation
                || cached_power != power
                || cached_source_angle != source_angle) {
                max_distance = std::sqrt(power * attenuation) / std::tan(source_angle);
                cached_attenuation = attenuation;
                cached_power = power;
                cached_source_angle = source_angle;
            }
        }

        float GetMaxDistance() const { return max_distance; }

    private:
        float max_distance = 0.0f;
        float cached_attenuation = -1.0f;
        float cached_power = -1.0f;
        float cached_source_angle = -1.0f;
    } light_data;
    bool needsUpdate = true;
};

struct SphereLightComponent {
    struct SphereLightData {
        float source_radius = 0;
        float r = 1, g = 1, b = 1;
        float power = 1;
        float attenuation = 1.0f;

        SphereLightData(
            float source_radius = 0,
            float r = 1, float g = 1, float b = 1,
            float power = 1,
            float attenuation = 1.0f
        )
            : source_radius(source_radius),
            r(r), g(g), b(b),
            power(power),
            attenuation(attenuation) {
        }

        void ResolveDistance() {
            if (cached_attenuation != attenuation || cached_power != power) {
                max_distance = std::sqrt(power * attenuation);
                cached_attenuation = attenuation;
                cached_power = power;
            }
        }

        float GetMaxDistance() const { return max_distance; }

    private:
        float max_distance = 0.0f;
        float cached_attenuation = -1.0f;
        float cached_power = -1.0f;
    } light_data;
    bool needsUpdate = true;
};

struct DirectLightComponent {
    struct DirectLightData {
        float dir_x = 0, dir_y = -1, dir_z = 0;
        float r = 1, g = 1, b = 1;
        float power = 1;

        float center_x = 0, center_y = 0, center_z = 0;
        float half_extent = 20.0f;
        float half_depth = 20.0f;

        static constexpr int MAX_CASCADES = 4;
        int   cascade_count = 3;
        float cascade_ratio = 3.0f;

        float CascadeExtent(int c) const {
            float e = half_extent;
            for (int k = 0; k < c; ++k) e *= cascade_ratio;
            return e;
        }

        float CascadeDepth(int c) const {
            float e = half_depth;
            for (int k = 0; k < c; ++k) e *= cascade_ratio;
            return e;
        }

        float CascadeFar(int c) const { return 2.0f * CascadeDepth(c); }

        DirectLightData(
            float dir_x = 0, float dir_y = -1, float dir_z = 0,
            float r = 1, float g = 1, float b = 1,
            float power = 1,
            float center_x = 0, float center_y = 0, float center_z = 0,
            float half_extent = 20.0f,
            float half_depth = 20.0f,
            int cascade_count = 3,
            float cascade_ratio = 3.0f)
            : dir_x(dir_x), dir_y(dir_y), dir_z(dir_z),
            r(r), g(g), b(b),
            power(power),
            center_x(center_x), center_y(center_y), center_z(center_z),
            half_extent(half_extent), half_depth(half_depth),
            cascade_count(cascade_count), cascade_ratio(cascade_ratio) {
        }
    } light_data;
    bool needsUpdate = true;
};
struct ShadowCasterComponent{};

struct ShadowComponent {};

struct DrawComponent {
	bool     visible = true;
	float    alpha   = 1.0f;
	uint32_t flags   = 0;
};

struct UIComponent {};

struct UITextComponent {
	std::vector<uint32_t> glyphs;
	uint32_t              font = 0;
};

struct TestComponent {};
