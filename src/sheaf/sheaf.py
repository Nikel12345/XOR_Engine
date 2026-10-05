# -*- coding: utf-8 -*-
"""Писатель формата Sheaf на Python (docs/sheaf.md) для генераторов сцен. Только стандартная
библиотека.

Способ записи поля выбирается так же, как у sheaf::Writer (Sheaf.cpp): те же кандидаты в том же
порядке и самый короткий из них. Поэтому файл отсюда совпадает байт в байт с тем, что даёт
C++-писатель на тех же строках и таблицах.

    w = sheaf.Writer()
    t = w.table(rows=2)
    c = t.component("Transform")
    c.field("w", sheaf.F32, [1.5, -3.0])
    t.component("Shadow")                               # тег: компонент без полей
    m = t.component("Renderable")
    m.field("mat_lod0", sheaf.STR, [["a", None], ["b"]], flags=sheaf.LIST | sheaf.NULLABLE)
    open("scene.sheaf", "wb").write(w.finish())

Значения передаются как есть: числа, bool, строки. У поля-списка значение объекта — список; None —
отсутствующее значение поля с флагом NULLABLE.
"""

import struct
import sys
from array import array

F32, U32, I32, U8, BOOL, STR, REF = range(7)
LIST, NULLABLE = 1, 2
_RAW, _CONST, _DEFAULTS, _DICT = range(4)

# Как kDictScalarLimit в Sheaf.cpp: дальше номер весит столько же, сколько значение.
_DICT_SCALAR_LIMIT = 0x10000

assert array("I").itemsize == 4 and array("H").itemsize == 2


def _u32(v):
    return struct.pack("<I", v)


def _little(a):
    if sys.byteorder == "big":
        a = array(a.typecode, a)
        a.byteswap()
    return a.tobytes()


def _byte_values(vals):
    # bytes(array) отдал бы память массива — по 4 байта на элемент, а не по байту на значение.
    return bytes(vals.tolist() if isinstance(vals, array) else vals)


def _values_bytes(t, vals):
    if t in (U8, BOOL):
        return _byte_values(vals)
    return _little(vals if isinstance(vals, array) and vals.typecode == "I" else array("I", vals))


def _bitmap(bits, n):
    out = bytearray((n + 7) // 8)
    for i, b in enumerate(bits):
        if b:
            out[i >> 3] |= 1 << (i & 7)
    return out


class _Column:
    def __init__(self, name, type_, flags, default, values, lengths, present):
        self.name, self.type, self.flags, self.default = name, type_, flags, default
        self.values, self.lengths, self.present = values, lengths, present

    def first(self):
        out, acc = array("I"), 0
        for n in self.lengths:
            out.append(acc)
            acc += n
        return out


class Component:
    def __init__(self, writer, rows, name):
        self._writer, self._rows, self.name, self.fields = writer, rows, name, []

    def field(self, name, type_, values, default=None, flags=0):
        """Колонка поля. default — значение, которое у поля обычное (у списка не пишется); его
        берут из кода компонента, без него — ноль своего типа."""
        if flags & ~(LIST | NULLABLE):
            raise ValueError("%s.%s: неизвестные флаги %d" % (self.name, name, flags))
        values = list(values)
        if len(values) != self._rows:
            raise ValueError("%s.%s: %d значений на %d объектов" % (self.name, name, len(values), self._rows))
        w = self._writer
        def_bits = 0 if flags & LIST else w._bits(type_, [default if default is not None else _zero(type_)])[0]
        lengths = None
        if flags & LIST:
            lengths = array("I", (len(v) for v in values))
            values = [x for v in values for x in v]
        present = None
        if flags & NULLABLE:
            present = bytearray(v is not None for v in values)
            values = [_zero(type_) if v is None else v for v in values]
        elif any(v is None for v in values):
            raise ValueError("%s.%s: None у поля без NULLABLE" % (self.name, name))
        bits = w._bits(type_, values, present)
        self.fields.append(_Column(name, type_, flags, def_bits, bits, lengths, present))
        return self


def _zero(t):
    return "" if t == STR else 0


class Table:
    def __init__(self, writer, rows):
        self._writer, self.rows, self.components = writer, rows, []

    def component(self, name):
        c = Component(self._writer, self.rows, name)
        self.components.append(c)
        return c


class Writer:
    def __init__(self):
        self.strings = []
        self._index = {}
        self._tables = []

    def intern(self, s):
        i = self._index.get(s)
        if i is None:
            i = len(self.strings)
            self._index[s] = i
            self.strings.append(s)
        return i

    def table(self, rows):
        """Таблица-архетип. Порядок таблиц в файле — порядок вызовов; сквозные номера ref считаются
        по нему же."""
        t = Table(self, rows)
        self._tables.append(t)
        return t

    def _bits(self, t, vals, present=None):
        if t == F32:
            return array("I", array("f", vals).tobytes())
        if t == I32:
            return array("I", array("i", vals).tobytes())
        if t in (U32, REF):
            return array("I", vals)
        if t == U8:
            a = array("I", vals)
            if a and max(a) > 0xFF:
                raise ValueError("u8 больше 255")
            return a
        if t == BOOL:
            return array("I", (1 if v else 0 for v in vals))
        if t == STR:
            # Отсутствующее значение не интернируется: строки, которой нет ни у кого, в файле не будет.
            if present is None:
                return array("I", (self.intern(s) for s in vals))
            return array("I", (self.intern(s) if p else 0 for s, p in zip(vals, present)))
        raise ValueError("неизвестный тип %d" % t)

    def finish(self):
        body = bytearray(_u32(len(self._tables)))
        for t in self._tables:
            enc = [_encode(col, t.rows) for comp in t.components for col in comp.fields]
            body += _u32(t.rows) + _u32(len(t.components))
            k = 0
            for comp in t.components:
                body += _u32(self.intern(comp.name)) + _u32(len(comp.fields))
                for col in comp.fields:
                    body += _u32(self.intern(col.name)) + bytes((col.type, col.flags, enc[k][0]))
                    k += 1
                    if not col.flags & LIST:
                        body += _values_bytes(col.type, [col.default])
            for _, data in enc:
                body += data
        out = bytearray(b"SHEF") + _u32(len(self.strings))
        for s in self.strings:
            b = s.encode("utf-8")
            out += _u32(len(b)) + b
        out += body
        self._tables = []
        return bytes(out)


def _write_raw(c, rows=None):
    """Способ 0 для строк rows (None — все по порядку)."""
    out = bytearray()
    lst, nul = c.flags & LIST, c.flags & NULLABLE
    if rows is None:
        if lst:
            out += _values_bytes(U32, c.lengths)
        if nul:
            out += _bitmap(c.present, len(c.present))
            out += _values_bytes(c.type, [v for v, p in zip(c.values, c.present) if p])
        else:
            out += _values_bytes(c.type, c.values)
        return out
    if lst:
        first = c.first()
        out += _values_bytes(U32, [c.lengths[r] for r in rows])
        cells = [i for r in rows for i in range(first[r], first[r] + c.lengths[r])]
    else:
        cells = list(rows)
    if nul:
        out += _bitmap((c.present[i] for i in cells), len(cells))
        cells = [i for i in cells if c.present[i]]
    out += _values_bytes(c.type, [c.values[i] for i in cells])
    return out


def _ids_bytes(ids, width):
    if width == 1:
        return _byte_values(ids)
    return _little(array("H" if width == 2 else "I", ids))


def _dict_header(k):
    width = 1 if k <= 0x100 else 2 if k <= 0x10000 else 4
    return width, _u32(k) + bytes((width,))


def _by_uniques_scalar(c, rows, cands):
    # dict.fromkeys хранит значения в порядке первого появления — тот же порядок уникальных, что у
    # EncodeByUniques.
    uniq = dict.fromkeys(c.values)
    if len(uniq) > _DICT_SCALAR_LIMIT:
        return
    if len(uniq) == 1:
        cands.append((_CONST, _values_bytes(c.type, [c.values[0]])))
    if len(uniq) == rows:
        return
    index = {v: i for i, v in enumerate(uniq)}
    width, data = _dict_header(len(uniq))
    data += _values_bytes(c.type, list(uniq))
    data += _ids_bytes(array("I", map(index.__getitem__, c.values)), width)
    cands.append((_DICT, data))


def _by_uniques_rows(c, rows, cands):
    lst, nul = c.flags & LIST, c.flags & NULLABLE
    first = c.first() if lst else None
    index, uniq, ids = {}, [], array("I")
    for r in range(rows):
        b, n = (first[r], c.lengths[r]) if lst else (r, 1)
        if nul:
            key = (n,) + tuple(c.values[i] if c.present[i] else None for i in range(b, b + n))
        else:
            key = (n,) + tuple(c.values[b:b + n])
        i = index.get(key)
        if i is None:
            i = len(uniq)
            index[key] = i
            uniq.append(r)
        ids.append(i)
    if len(uniq) == 1:
        cands.append((_CONST, _write_raw(c, uniq)))
    if len(uniq) == rows:
        return
    width, data = _dict_header(len(uniq))
    data += _write_raw(c, uniq)
    data += _ids_bytes(ids, width)
    cands.append((_DICT, data))


def _encode(c, rows):
    cands = [(_RAW, _write_raw(c))]
    if rows > 0:
        if c.flags:
            _by_uniques_rows(c, rows, cands)
        else:
            _by_uniques_scalar(c, rows, cands)
    if not c.flags:
        d = c.default
        own = [v for v in c.values if v != d]
        cands.append((_DEFAULTS, _bitmap((v != d for v in c.values), rows) + _values_bytes(c.type, own)))
    best = cands[0]
    for e in cands[1:]:
        if len(e[1]) < len(best[1]):
            best = e
    return best
