#ifndef SPARSE_RANK_HLSL
#define SPARSE_RANK_HLSL

#define SPARSE_CHANNEL(NAME, REG, SPACE)                                    \
    StructuredBuffer<uint2> NAME##Words : register(REG, SPACE);             \
    int NAME##Rank(int row)                                                 \
    {                                                                       \
        if (row < 0) return -1;                                             \
        const uint  b = uint(row) & 31u;                                    \
        const uint2 w = NAME##Words[uint(row) >> 5u];                       \
        if (((w.x >> b) & 1u) == 0u) return -1;                             \
        return int(w.y + countbits(w.x & ((1u << b) - 1u)));                \
    }

#endif
