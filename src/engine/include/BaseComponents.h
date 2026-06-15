#pragma once
#include <vector>
#include <unordered_map>
#include <typeindex>
#include <memory>
#include <SDL3/SDL.h>

struct ModelData;
struct TextureData;
struct Material;

using f_restrict_pointer = float const* __restrict;
using Entity = uint32_t;

template<typename, typename = void>
struct is_soa : std::false_type {};

template<typename T>
struct is_soa<T, std::void_t<typename T::soa_tag>> : std::true_type {};
template<typename, typename = void>
struct has_related_soa : std::false_type {};

template<typename T>
struct has_related_soa<T, std::void_t<typename T::related_soa>> : std::true_type {};

template<typename Derived>
struct SoAProxyAddable {
    template<typename Proxy>
    auto add(const Proxy& proxy)
        -> decltype(proxy.emplace_to(static_cast<Derived&>(*this)), void()) {
        proxy.emplace_to(static_cast<Derived&>(*this));
    }

    void swap_remove(size_t i) {
        std::apply([&](auto&... col) {
            (..., ([&] {
                const size_t last = col.size() - 1;
                if (i != last) col[i] = std::move(col[last]);
                col.pop_back();
                }()));
            }, static_cast<Derived&>(*this).columns());       // columns() � �� Derived
    }
};

//-----------------------Acicelerations-----------------------//
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
//-----------------------Velocities-----------------------//
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

//-----------------------Positions-----------------------//
struct Positions : SoAProxyAddable<Positions> {
    using soa_tag = void;              // <� ������ SoA

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

struct ParentComponent {
    Entity parent;
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

struct MassComponent {
    float mass;
};


struct TextureComponent {
    TextureData* texture;
    TextureData* normal;
};


struct ModelComponent {
    ModelData* model;
};

// Порядок расположения материалов должен соответствовать порядку сабмешей в модели, поскольку индекс материала в сабмеше используется для доступа к материалу
// Order of materials must correspond to the order of submeshes in the model, as the material index in the submesh is used to access the material
struct MaterialComponent {
    std::vector<Material*> materials;
};

enum LightTypes {
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
        float source_radius;
        float dir_x, dir_y, dir_z;
        float source_angle;
        float r, g, b;
        float power;
    } light_data;
    bool needsUpdate;
};
struct ShadowCasterComponent{};

struct ShadowComponent {};

struct TestComponent {};


struct IComponentArray {
    virtual ~IComponentArray() = default;
    virtual void swap_remove(size_t i) = 0;
};

// AoS (�� ���������)
template<typename T, typename = void>
struct ComponentArray : IComponentArray {
    std::vector<T> data;

    void add(const T& v) { data.push_back(v); }
    T& operator[](size_t i) { return data[i]; }
    size_t size() const { return data.size(); }

    void swap_remove(size_t i) override {
        const size_t last = data.size() - 1;
        if (i != last) data[i] = std::move(data[last]);  // ���� �� self-move
        data.pop_back();
    };
};


// SoA (���� � T ���� soa_tag)
template<typename T>
struct ComponentArray<T, std::enable_if_t<is_soa<T>::value>> : IComponentArray {
    T data;
    template<typename Proxy>
    auto add(const Proxy& proxy) -> decltype(data.add(proxy), void()) { data.add(proxy); }
    size_t size() const { return data.size(); }

    void swap_remove(size_t i) override { data.swap_remove(i); }
};


struct Archetype {
    std::vector<Entity> entities;
    std::unordered_map<std::type_index, std::unique_ptr<IComponentArray>> components;
    uint32_t render_instance_base = 0;

    template<typename T>
    ComponentArray<T>* get_array() {
        auto it = components.find(std::type_index(typeid(T)));
        if (it == components.end()) {
            //SDL_Log("Archetype::get_array: ComponentArray for type %s not found", typeid(T).name());
            return nullptr;
        };
        return static_cast<ComponentArray<T>*>(it->second.get());
    }

    template<typename T>
    void ensure_component() {
        auto idx = std::type_index(typeid(T));
        if (!components.count(idx))
            components[idx] = std::make_unique<ComponentArray<T>>();
    }
    void swap_remove(size_t i) {
        for (auto& [type, arr] : components)
            arr->swap_remove(i);
    }
};
