// Линейная карта расстояний (distance shadow map).
// Вместо нелинейной аппаратной глубины проекции храним евклидово расстояние от
// источника до фрагмента, нормированное на far. В view-space источник находится
// в начале координат, поэтому расстояние = length(viewPos).
//
// Зачем: евклидово расстояние непрерывно поперёк граней кубической карты (нет
// швов между фрустумами) и имеет равномерную точность на любой дистанции — bias
// ведёт себя одинаково и вблизи, и вдали от источника.
//
// Эти uniform'ы приходят из ShadowPushData через PushFragment (слот 0).
cbuffer ShadowCameraFS : register(b0, space3) {
    uint  fs_cameraIndex;
    float fs_farRange;
    uint  fs_is_ortho;   // 1 — directional (ortho): линейная осевая глубина
};

// Пишем SV_Depth вручную => аппаратный Early-Z и polygon depth-bias на этот
// проход не действуют (bias делаем в сравнении на main-проходе).
float main(float3 viewPosWS : TEXCOORD0) : SV_Depth
{
    // Directional (ortho): лучи параллельны, «расстояния до источника» нет —
    // храним ЛИНЕЙНУЮ глубину вдоль оси света. В view-space камера смотрит вдоль -Z,
    // поэтому -viewPosWS.z ∈ [0, far]. Перпендикулярная компонента (как в length())
    // тут только бы исказила сравнение, ведь сечение бокса постоянно.
    if (fs_is_ortho != 0)
        return -viewPosWS.z / fs_farRange;

    // Spot/Sphere (perspective): евклидова дистанция — равномерная точность и без швов.
    return length(viewPosWS) / fs_farRange;
}
