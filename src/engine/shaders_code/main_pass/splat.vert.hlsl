// Вершинник СПЛАТ-прохода: одна точка на объект вместо его геометрии.
//
// Сплат — последний уровень LOD: модель из одной вершины и одного индекса, поэтому вершинник
// запускается ровно раз на инстанс, а a_pos — точка объекта, в которую он схлопнут.
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
// SV_InstanceID = first_instance + i, а first_instance команды указывает на её записи в Rows.
StructuredBuffer<int>      Rows           : register(t1, space0);

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

    int row = Rows[input.instanceID];
    if (row < 0) {
        // Вырожденная позиция за клип-плоскостью. Голый return оставил бы SV_Position UB.
        o.sv_pos = float4(2.0, 2.0, 2.0, 1.0);
        return o;
    }

    float4 worldPos = mul(ModelMatrixBlock[row], float4(input.a_pos, 1.0));
    o.sv_pos = mul(Camera[0].proj, mul(Camera[0].view, worldPos));
    return o;
}
