// Вершинник СПЛАТ-прохода: одна точка на объект вместо его геометрии.
//
// Проход рисуется командой с num_indices == 1 (RenderPassStep::override_index_count), поэтому
// вершинник запускается ровно раз на инстанс. До какой именно вершины сабмеша дотянулся этот
// единственный индекс — неважно: сплат-проход по построению получает только объекты, чей экранный
// радиус меньше порога, а у такого объекта любая его вершина лежит в том же пикселе, что и центр.
// Поэтому BoundSpheres здесь не читается вовсе.
//
// ЗАГЛУШКА: цвет константный (splat.frag.hlsl). Настоящий сплат обязан выполнять ФРАГМЕНТНИК
// МАТЕРИАЛА — только тогда тень, прямой свет, окружение и пользовательские слоты учитываются, а не
// подменяются предпосчитанным средним. Тогда этому вершиннику придётся заполнять чужой VSOutput
// синтезированными UV/нормалью/тангентом, и вся работа будет здесь.

struct VSInput {
    float3 a_pos      : POSITION;
    uint   instanceID : SV_InstanceID;
};

StructuredBuffer<float4x4> ModelMatrixBlock : register(t0, space0);
// out_pib: адрес куска записей команды лежит в её first_instance, а SV_InstanceID = first_instance + i,
// поэтому индексируем напрямую. -1 = инстанс отсечён каллингом.
StructuredBuffer<int>      OutPib           : register(t1, space0);

struct CameraData { float4x4 view; float4x4 proj; };
StructuredBuffer<CameraData> Camera : register(t2, space0);

struct VSOutput {
    float4 sv_pos : SV_Position;
    // Vulkan размер точки НЕ подразумевает: не записал — не определён. Единица = ровно пиксель,
    // больше субпиксельному объекту и не нужно.
    [[vk::builtin("PointSize")]] float psize : PSIZE;
};

VSOutput main(VSInput input)
{
    VSOutput o;
    o.psize = 1.0;

    int row = OutPib[input.instanceID];
    if (row < 0) {
        // Отсечён: вырожденная позиция за клип-плоскостью. Голый return оставил бы SV_Position UB.
        o.sv_pos = float4(2.0, 2.0, 2.0, 1.0);
        return o;
    }

    float4 worldPos = mul(ModelMatrixBlock[row], float4(input.a_pos, 1.0));
    o.sv_pos = mul(Camera[0].proj, mul(Camera[0].view, worldPos));
    return o;
}
