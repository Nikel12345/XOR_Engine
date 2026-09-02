#!/usr/bin/env python3
"""
Генератор моделей небоскрёбов. Один запуск = одна модель (два .bin в game/models/buildings).

Сделано: форма (сетка 3x3), высота (одна на все блоки), крыша (элемент на блок).
Дальше по плану: декор -> окна.

Формат вершины — пул PosUVNorm движка (44 байта, интерлив):
  float x, y, z | float u, v | float nx, ny, nz | float tx, ty, tz
Формат файла — как у scripts/model_loader/convert_model.py:
  вершинный:  uint32 submesh_count, затем submesh_count x (5 x uint32), затем вершины;
  индексный:  сырой массив uint32.
"""

import math
import random
import struct
from collections import deque
from pathlib import Path

# ── Параметры формы ───────────────────────────────────────────────────────────
GRID = 3          # сетка GRID x GRID ячеек
CELL = 10.0       # сторона ячейки в юнитах мира
PICK_CHANCE = 0.5 # шанс, что очередной кандидат BFS попадёт в форму (иначе просто пропущен)

# ── Параметры высоты ──────────────────────────────────────────────────────────
# Высота МЕРИТСЯ В ЭТАЖАХ и только потом переводится в юниты: этаж — это шаг, к которому
# обязаны привязаться ряды окон, поэтому непрерывной высоты у блока быть не должно.
FLOOR_H = 3.5            # высота этажа в юнитах
FLOORS = (12, 40)        # этажей у здания; высота ОДНА на все блоки

# ── Вертикальные участки фасада ───────────────────────────────────────────────
# Фасад режется по высоте на СЕГМЕНТЫ, и это основной примитив, а не частный случай цоколя:
# у сегмента свой диапазон высоты, свой сабмеш, свой вынос наружу и (дальше по плану) своя
# сетка со своим набором декора. Цоколь ЗАБИРАЕТ нижние этажи у корпуса, венец — верхние,
# поэтому ряды окон живут только в диапазоне корпуса.
PODIUM_MIN_H = 5.0       # МИНИМАЛЬНАЯ высота цоколя в юнитах: вестибюль ниже неё читается
                         # не как цоколь, а как случайная ступенька. Задана в юнитах, а не в
                         # этажах, но округляется ВВЕРХ до целого числа этажей — высота здания
                         # обязана оставаться кратной шагу этажа, иначе поедет сетка окон.
PODIUM_MAX_FLOORS = 3
PODIUM_INSET = 0.4       # цоколь УЖЕ корпуса: корпус нависает над ним этим свесом
CROWN_FLOORS = (0, 2)    # этажей между верхним рядом окон и крышей
TECH_FLOORS = 1          # высота технического пояса в этажах
TECH_INSET = 0.3         # пояс уже корпуса — как цоколь, только в середине здания
TECH_MIN_GAP = 5         # этажей до другого пояса, до крыши и до цоколя
PARAPET_H = 1.4          # высота стены крыши над плоскостью крыши
PARAPET_T = 0.35         # её толщина внутрь от плоскости стены

# Размер панели, на которые режется ЛЮБАЯ поверхность стены (см. emit_face_panels): и стены,
# и полки выреза, и лента парапета. Это ГЛАВНАЯ цена модели: панель стоит 4 вершины, а число
# панелей растёт как 1/размер^2 — 85% вершин здания уходит на них, а не на его форму.
# Размер обязан отвечать тому, что НАРИСОВАНО на текстуре: один этаж на картинку -> 1,
# два этажа -> 2. Меряется в этажах, чтобы ряды панелей ложились на этажи без подгонки.
PANEL_FLOORS = 1

# ── Окна ──────────────────────────────────────────────────────────────────────
# Окно занимает ячейку сетки ЦЕЛИКОМ: рама, откос и поля стены нарисованы в самом тайле,
# поэтому "простенок между окнами" — вопрос текстуры, а не геометрии, и фасад стоит ровно
# столько же квадов, сколько стоила бы голая стена. Глубины нет: стекло вровень со стеной.
WINDOW_SHEET = (4, 4)      # раскладка листа вариантов: 16 тайлов
WINDOW_TEXTURE = "src/game/textures/assets/windows.jpg"
WINDOW_TILE_PAD = 0.002    # гуттер внутрь тайла в UV: без него мип затянет соседний тайл в край окна
WINDOW_TILE_ASPECT_FALLBACK = 1.49   # если текстуры нет на месте — аспект тайла windows.jpg


def jpeg_size(path):
    """Ширина/высота JPEG из маркера SOF. Аспект ячейки обязан идти ОТ ТЕКСТУРЫ, а не быть
    вписанной константой: лист не квадратный, и разъезд этих двух чисел растянет окно."""
    try:
        d = open(path, "rb").read()
    except OSError:
        return None
    i = 2
    while i + 9 < len(d):
        if d[i] != 0xFF:
            i += 1
            continue
        m = d[i + 1]
        if m in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):
            return ((d[i + 7] << 8) | d[i + 8], (d[i + 5] << 8) | d[i + 6])
        i += 2 + ((d[i + 2] << 8) | d[i + 3])
    return None


def window_tile_aspect():
    px = jpeg_size(str(Path(__file__).resolve().parents[2] / WINDOW_TEXTURE))
    if not px:
        return WINDOW_TILE_ASPECT_FALLBACK
    return (px[0] / WINDOW_SHEET[0]) / (px[1] / WINDOW_SHEET[1])

# Номер сабмеша = индекс материала у сущности, поэтому таблица ФИКСИРОВАНА: слот, однажды
# получивший номер, его не меняет — иначе правка генератора молча переставила бы материалы на
# уже расставленных в сцене зданиях.
#
# Отсюда правило: новый слот ДОПИСЫВАЕТСЯ В КОНЕЦ, а не вставляется. Номер, зарезервированный
# под ещё не сделанный слот, оставляет ДЫРКУ в нумерации, а движок берёт материал по номеру
# сабмеша, а не по его порядку в файле: сущности пришлось бы держать лишний материал-заглушку,
# иначе сабмеш за дыркой уходит в "material_index out of range" и не рисуется вовсе.
SUBMESH_SLOTS = {"wall": 0, "podium": 1, "tech": 2, "windows": 3}

VERTEX_STRUCT = "fffffffffff"   # 11 float = 44 байта
SUBMESH_ENTRY = "IIIII"         # vOffset, iOffset, vCount, iCount, material_index

OUT_DIR = Path(__file__).resolve().parents[2] / "src" / "game" / "models" / "buildings"
# Модель ОДНА и перезаписывается каждым запуском: имя фиксированное, ссылка на неё в манифестах
# сцены не протухает. Библиотека вариантов — не сейчас.
MODEL_NAME = "building"

# Грани единичного куба в +-1-пространстве: c — стартовый угол, U/V — рёбра, N — внешняя нормаль.
# cross(U, V) = N, поэтому обход p0=c, p1=c+U, p2=c+U+V, p3=c+V даёт CCW наружу.
# Таблица скопирована из движкового генератора "cube" (Engine.cpp) — обмотка и развёртка обязаны
# совпадать с ней, иначе фасады окажутся вывернуты относительно остальных мешей сцены.
FACES = [
    ((1, -1, 1),  (0, 0, -2), (0, 2, 0),  (1, 0, 0)),    # +X
    ((-1, -1, -1),(0, 0, 2),  (0, 2, 0),  (-1, 0, 0)),   # -X
    ((-1, 1, 1),  (2, 0, 0),  (0, 0, -2), (0, 1, 0)),    # +Y
    ((-1, -1, -1),(2, 0, 0),  (0, 0, 2),  (0, -1, 0)),   # -Y
    ((-1, -1, 1), (2, 0, 0),  (0, 2, 0),  (0, 0, 1)),    # +Z
    ((1, -1, -1), (-2, 0, 0), (0, 2, 0),  (0, 0, -1)),   # -Z
]
QUAD_UV = ((0, 0), (1, 0), (1, 1), (0, 1))

# ── Паттерны крыши ────────────────────────────────────────────────────────────
# Элемент крыши целится в ЦЕНТР блока; всё смещение от центра сидит в самом паттерне (pivot),
# а не выбирается на месте — экземпляры одного паттерна одинаковы, случаен только выбор
# паттерна на блок. pivot = (dx, dz) в плоскости крыши.
# Коробка = (dx, dy, dz, hx, hy, hz): смещение центра от пивота и полу-размеры. dy отсчитан
# от плоскости крыши, поэтому dy == hy ставит коробку РОВНО на крышу.
# Цилиндр = (радиус, высота, сегментов).
# limit — сколько таких элементов допустимо НА ЗДАНИЕ; None = без ограничения.
ROOF_PATTERNS = {
    # Пустая крыша. Ограничения нет: остальные паттерны упрутся в свои лимиты, и добирать
    # оставшиеся блоки должно быть чем.
    "none": {"pivot": (0.0, 0.0), "cylinder": None, "boxes": (), "limit": None},
    # Вертолётная площадка: очень низкий широкий цилиндр, пивот 0 — она и есть центр крыши.
    "helipad": {"pivot": (0.0, 0.0), "cylinder": (4.0, 0.35, 24), "boxes": (), "limit": 1},
    # Вентиляция: группа мелких коробок, сдвинута с центра — центр оставлен под площадку/антенну.
    "vents": {"pivot": (-2.0, 1.8), "cylinder": None, "limit": 3, "boxes": (
        (0.0, 0.9, 0.0, 0.9, 0.9, 0.9),
        (2.0, 0.7, 0.4, 0.7, 0.7, 0.7),
        (0.6, 1.3, -1.8, 0.5, 1.3, 0.5),
        (-1.6, 0.6, 1.2, 0.6, 0.6, 1.0),
    )},
    # Выход на крышу: коробка, вытянутая в плане — так она читается как лестничная пристройка,
    # а не как ещё один вентблок. У края блока по той же причине.
    "access": {"pivot": (2.4, -2.4), "cylinder": None, "limit": 2, "boxes": (
        (0.0, 1.4, 0.0, 2.4, 1.4, 1.3),
    )},
    # Антенна: высокая узкая мачта, тоже у края — по центру она спорила бы с площадкой.
    "antenna": {"pivot": (-2.8, -2.8), "cylinder": None, "limit": 2, "boxes": (
        (0.0, 7.0, 0.0, 0.25, 7.0, 0.25),
    )},
}

# Сосед по грани для каждой записи FACES, в клетках сетки (dx, dz). None = грань наружу всегда.
FACE_NEIGHBOUR = [(1, 0), (-1, 0), None, None, (0, 1), (0, -1)]

NEIGHBOUR_OFFSETS = [(dx, dz) for dx in (-1, 0, 1) for dz in (-1, 0, 1) if (dx, dz) != (0, 0)]


# ── Этап "форма": какие ячейки сетки заняты ───────────────────────────────────
def pick_footprint(rng):
    """BFS от случайной стартовой ячейки. Каждая ещё не посещённая соседка (включая диагональную)
    решается ОДНИМ броском: выбрана — попадает в форму и растит дальше, пропущена — рост в её
    сторону обрывается. visited помечается в ОБОИХ случаях, поэтому один и тот же кандидат не
    переспрашивается с другой стороны, а обход конечен. Форма всегда связная: каждая ячейка
    пришла от уже выбранного соседа."""
    start = (rng.randrange(GRID), rng.randrange(GRID))
    cells = {start}
    visited = {start}
    queue = deque([start])
    while queue:
        gx, gz = queue.popleft()
        for dx, dz in NEIGHBOUR_OFFSETS:
            nx, nz = gx + dx, gz + dz
            if not (0 <= nx < GRID and 0 <= nz < GRID) or (nx, nz) in visited:
                continue
            visited.add((nx, nz))
            if rng.random() < PICK_CHANCE:
                cells.add((nx, nz))
                queue.append((nx, nz))
    return cells


# ── Этап "высота" ─────────────────────────────────────────────────────────────
def assign_heights(rng):
    """Этажей у здания — одно число на ВСЕ блоки. Меряем в этажах, а не в юнитах: этаж —
    шаг, к которому обязаны привязаться ряды окон, поэтому высота обязана быть кратна ему."""
    return rng.randint(*FLOORS)


def tech_belt_floors(body_f, rng):
    """Этажи корпуса (индексы от его низа), с которых начинаются технические пояса.

    Количество случайно, но расстановка выбирается РАВНОМЕРНО среди всех допустимых сразу,
    без цикла отказов: k позиций с минимальным шагом D из M мест — это в точности k
    произвольных позиций из M-(k-1)(D-1), у которых i-я сдвинута на i*(D-1). Поэтому здание,
    куда влезает ровно один пояс, не приходится перебрасывать десятки раз.

    Границы: до цоколя и до крыши — те же TECH_MIN_GAP этажей, что и между поясами. Меряем
    до верха КОРПУСА, а венец (если он есть) идёт сверх того — запас только растёт."""
    T, G = TECH_FLOORS, TECH_MIN_GAP
    M = body_f - 2 * G - T + 1
    if M <= 0:
        return []
    D = G + T
    k = rng.randint(0, (M + D - 1) // D)
    if k == 0:
        return []
    picks = sorted(rng.sample(range(M - (k - 1) * (D - 1)), k))
    return [G + q + i * (D - 1) for i, q in enumerate(picks)]


# ── Этап "окна" ───────────────────────────────────────────────────────────────
def assign_facade(rng):
    """Этап "окна": здание получает СИД раскладки. Сам выбор тайла считается хэшом на месте
    (см. window_tile) — сид лишь разводит здания, чтобы одинаковые формы не совпали окно в окно."""
    return rng.randrange(1 << 30)


def window_tile(seed, gx, gz, f, col, row):
    """Тайл конкретного окна — любой из 16. Строки листа НЕ семейства: на windows.jpg каждый
    тайл это отдельная квартира со своим светом, поэтому вариант берётся из всего листа.

    Выбор — ХЭШОМ от адреса окна, а не броском rng: эмиттеру меша не нужно тащить через себя
    генератор, а фасад воспроизводим при той же форме. seed разводит здания между собой."""
    h = (seed * 2246822519) ^ (gx * 73856093) ^ (gz * 19349663) ^ (f * 83492791)         ^ (col * 2654435761) ^ (row * 40503)
    h = (h ^ (h >> 13)) & 0x7FFFFFFF
    n = h % (WINDOW_SHEET[0] * WINDOW_SHEET[1])
    tx, ty = n % WINDOW_SHEET[0], n // WINDOW_SHEET[0]
    du, dv = 1.0 / WINDOW_SHEET[0], 1.0 / WINDOW_SHEET[1]
    p = WINDOW_TILE_PAD
    return (tx * du + p, ty * dv + p, (tx + 1) * du - p, (ty + 1) * dv - p)


# ── Этап "вертикальные участки" ───────────────────────────────────────────────
def vertical_segments(n_floors, rng):
    """Список сегментов снизу вверх. kind "slab" — участок стены (коробка блока, раздутая
    наружу на extrude); kind "parapet" — стена крыши, кольцо по открытым рёбрам блока.

    grid=True помечает сегмент, который дальше получит сетку окон. Сейчас такой один (корпус),
    но поле не лишнее: цоколю обещан свой декор, и он придёт тем же механизмом с другим набором."""
    # Цоколь есть ВСЕГДА: вариант "цоколя нет" на модели неотличим от цоколя нулевой высоты,
    # то есть от ошибки. Нужен безцокольный силуэт — это отдельный вид сегментации, а не ноль.
    podium_f = rng.randint(int(math.ceil(PODIUM_MIN_H / FLOOR_H)), PODIUM_MAX_FLOORS)
    crown_f = rng.randint(*CROWN_FLOORS)
    body_f = n_floors - podium_f - crown_f

    y = 0.0
    segs = []
    if podium_f:
        segs.append({"name": "podium", "kind": "slab", "slot": "podium", "grid": False,
                     "y0": y, "y1": y + podium_f * FLOOR_H, "extrude": -PODIUM_INSET,
                     "floors": podium_f})
        y = segs[-1]["y1"]
    # Корпус разрезается техническими поясами на куски; каждый кусок — обычный сегмент стены
    # с сеткой окон, каждый пояс — сегмент со своим слотом (значит, своим материалом) и вносом.
    cursor = 0
    for start in tech_belt_floors(body_f, rng):
        segs.append({"name": "body", "kind": "slab", "slot": "wall", "grid": True,
                     "y0": y, "y1": y + (start - cursor) * FLOOR_H, "extrude": 0.0,
                     "floors": start - cursor})
        y = segs[-1]["y1"]
        segs.append({"name": "tech", "kind": "slab", "slot": "tech", "grid": False,
                     "y0": y, "y1": y + TECH_FLOORS * FLOOR_H, "extrude": -TECH_INSET,
                     "floors": TECH_FLOORS})
        y = segs[-1]["y1"]
        cursor = start + TECH_FLOORS
    segs.append({"name": "body", "kind": "slab", "slot": "wall", "grid": True,
                 "y0": y, "y1": y + (body_f - cursor) * FLOOR_H, "extrude": 0.0,
                 "floors": body_f - cursor})
    y = segs[-1]["y1"]
    if crown_f:
        segs.append({"name": "crown", "kind": "slab", "slot": "wall", "grid": False,
                     "y0": y, "y1": y + crown_f * FLOOR_H, "extrude": 0.0, "floors": crown_f})
        y = segs[-1]["y1"]
    segs.append({"name": "parapet", "kind": "parapet", "slot": "wall", "grid": False,
                 "y0": y, "y1": y + PARAPET_H, "extrude": 0.0, "floors": 0})
    return segs


# ── Этап "крыша": по элементу на блок ─────────────────────────────────────────
def assign_roofs(cells, rng):
    """Ровно один элемент на блок. Выбор случаен среди тех паттернов, чей лимит на здание ещё
    не выбран: исчерпанный не «пропускает ход» (иначе крыша молча пустела бы), а выбывает из
    жеребьёвки, и блок получает что-то другое. "none" без лимита — им и добирается остаток."""
    used = {name: 0 for name in ROOF_PATTERNS}
    roofs = {}
    for cell in sorted(cells):
        free = [n for n, spec in ROOF_PATTERNS.items()
                if spec["limit"] is None or used[n] < spec["limit"]]
        pick = rng.choice(sorted(free))
        used[pick] += 1
        roofs[cell] = pick
    return roofs


# ── Меш ───────────────────────────────────────────────────────────────────────
def emit_face(verts, indices, f, center, half, uv=(0.0, 0.0, 1.0, 1.0)):
    """Одна грань по канону движкового куба: v-down развёртка, тангенс = направление U.

    center/half — коробка ИМЕННО этой грани. uv — подпрямоугольник текстуры (u0, v0, u1, v1),
    где (u0, v0) — ВЕРХНИЙ левый угол в текстурных координатах: этим же аргументом дальше
    выбирается тайл окна из листа вариантов. По умолчанию грань берёт текстуру целиком."""
    c, U, V, N = FACES[f]
    # Тангенс — направление U в МИРОВЫХ пропорциях (грани неквадратные, half по осям разный).
    tx, ty, tz = U[0] * half[0], U[1] * half[1], U[2] * half[2]
    tl = (tx * tx + ty * ty + tz * tz) ** 0.5
    if tl > 0.0:
        tx, ty, tz = tx / tl, ty / tl, tz / tl

    base = len(verts)
    for uq, vq in QUAD_UV:
        px = (c[0] + uq * U[0] + vq * V[0]) * half[0] + center[0]
        py = (c[1] + uq * U[1] + vq * V[1]) * half[1] + center[1]
        pz = (c[2] + uq * U[2] + vq * V[2]) * half[2] + center[2]
        u = uv[0] + uq * (uv[2] - uv[0])
        v = uv[1] + (1.0 - vq) * (uv[3] - uv[1])
        verts.append((px, py, pz, u, v, N[0], N[1], N[2], tx, ty, tz))
    indices += [base + 0, base + 1, base + 2, base + 0, base + 2, base + 3]


def emit_box(verts, indices, center, half, skip=(3,)):
    """Коробка целиком. По умолчанию пропускается -Y: элементы крыши стоят НА ней, их дно
    закрыто и совпало бы с ней плоскостью."""
    for f in range(6):
        if f not in skip:
            emit_face(verts, indices, f, center, half)


def emit_cylinder(verts, indices, center, r, h, seg):
    """Цилиндр без дна (стоит на крыше): боковина + верхняя крышка веером.
    Обмотка и развёртка — тот же канон, что у граней: CCW наружу, v-down, тангенс вдоль U."""
    cx, cy, cz = center
    ring = []
    for i in range(seg):
        a = 2.0 * math.pi * i / seg
        ring.append((math.cos(a), math.sin(a)))

    for i in range(seg):
        c0, s0 = ring[i]
        c1, s1 = ring[(i + 1) % seg]
        u0, u1 = i / seg, (i + 1) / seg
        base = len(verts)
        # Порядок низ-a0, верх-a0, верх-a1, низ-a1 даёт CCW снаружи (проверено check_mesh).
        quad = (((c0, s0), 0.0, u0, 1.0), ((c0, s0), h, u0, 0.0),
                ((c1, s1), h, u1, 0.0), ((c1, s1), 0.0, u1, 1.0))
        for (cs, sn), y, u, v in quad:
            # Нормаль радиальная, тангенс — вдоль растущего u, то есть по касательной кольца.
            verts.append((cx + r * cs, cy + y, cz + r * sn, u, v,
                          cs, 0.0, sn, -sn, 0.0, cs))
        indices += [base + 0, base + 1, base + 2, base + 0, base + 2, base + 3]

    # Крышка: веер от центра. Обход по УБЫВАЮЩЕМУ углу — +Y-грань канона идёт +X, затем -Z.
    top = len(verts)
    verts.append((cx, cy + h, cz, 0.5, 0.5, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0))
    for cs, sn in ring:
        verts.append((cx + r * cs, cy + h, cz + r * sn,
                      0.5 + cs * 0.5, 0.5 + sn * 0.5, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0))
    for i in range(seg):
        indices += [top, top + 1 + (i + 1) % seg, top + 1 + i]


def emit_roof_element(verts, indices, pattern, cx, roof_y, cz):
    """Один элемент на крышу блока: пивот паттерна сдвигает его от центра блока."""
    spec = ROOF_PATTERNS[pattern]
    px, pz = spec["pivot"]
    ox, oz = cx + px, cz + pz
    if spec["cylinder"]:
        r, h, seg = spec["cylinder"]
        emit_cylinder(verts, indices, (ox, roof_y, oz), r, h, seg)
    for dx, dy, dz, hx, hy, hz in spec["boxes"]:
        emit_box(verts, indices, (ox + dx, roof_y + dy, oz + dz), (hx, hy, hz))


def face_box(f, cx, cz, hspan, vspan, depth, offset=0.0):
    """center/half коробки, приклеенной к грани f блока с центром (cx, cz) в плане.

    hspan/vspan — МИРОВЫЕ интервалы прямоугольника (hspan по Z у граней +-X, по X у +-Z);
    offset двигает плоскость стены наружу (вынос сегмента), depth — толщина от неё: >0 наружу,
    <0 внутрь, 0 = плоский квад. Этим же хелпером дальше режутся проёмы и откосы окон — он
    и есть перевод "прямоугольник на стене" в коробку канона."""
    sign = 1.0 if f in (0, 4) else -1.0
    h0, h1 = hspan
    v0, v1 = vspan
    hc, hh = (h0 + h1) * 0.5, (h1 - h0) * 0.5
    nc = sign * (CELL * 0.5 + offset + depth * 0.5) + (cx if f in (0, 1) else cz)
    nh = abs(depth) * 0.5
    if f in (0, 1):
        return (nc, (v0 + v1) * 0.5, hc), (nh, (v1 - v0) * 0.5, hh)
    return (hc, (v0 + v1) * 0.5, nc), (hh, (v1 - v0) * 0.5, nh)


def slab_bounds(seg, cells, gx, gz):
    """Мировые габариты сегмента блока в плане.

    Вынос/внос сегмента применяется ТОЛЬКО на внешних сторонах: на общей с соседом стороне
    сегмент обязан быть вровень с границей блока. Иначе габариты соседей расходятся, и на
    стыке появляется наслоение (сегмент шире) или сквозная щель (уже) — а отсечение общей
    грани при этом продолжает считать её закрытой."""
    span = GRID * CELL
    cx = gx * CELL + CELL * 0.5 - span * 0.5
    cz = gz * CELL + CELL * 0.5 - span * 0.5
    e = {}
    for f, nb in enumerate(FACE_NEIGHBOUR):
        if nb is not None:
            e[f] = 0.0 if (gx + nb[0], gz + nb[1]) in cells else seg["extrude"]
    return (cx - CELL * 0.5 - e[1], cx + CELL * 0.5 + e[0],
            cz - CELL * 0.5 - e[5], cz + CELL * 0.5 + e[4], cx, cz, e)


def interval_diff(a, b):
    """a без b для отрезков: 0..2 куска."""
    out = []
    if b[0] > a[0]:
        out.append((a[0], min(b[0], a[1])))
    if b[1] < a[1]:
        out.append((max(b[1], a[0]), a[1]))
    return [(p, q) for p, q in out if q - p > 1e-6]


def emit_face_panels(verts, indices, f, center, half, target=None, pick=None):
    """Грань коробки, порезанная на панели размером ~target (по умолчанию — этаж), каждая
    со своим полным [0,1]. Единственный способ повторить текстуру: тайлить UV нельзя, атлас
    считает uv*scale+offset без frac.

    Сторона КОРОЧЕ панели берёт ДОЛЮ текстуры, а не сжимает в себя всю: иначе полоска-заплатка
    на стыке блоков или лента парапета показывают ту же картинку в другом масштабе, и на стыке
    с соседней панелью это видно как разрыв.

    Вершины соседних панелей не переиспользуются и не могут: у кромки одной v=0, у кромки
    следующей v=1 — позиции совпадают, UV нет. Переиспользуются значения UV, это и есть повтор.

    pick(col, row) -> прямоугольник тайла: так панель становится ОКНОМ и берёт свой вариант
    из листа. Доля для короткой стороны при этом считается ВНУТРИ тайла, а не поверх него."""
    tu, tv = target or (PANEL_FLOORS * FLOOR_H, PANEL_FLOORS * FLOOR_H)
    c, U, V, N = FACES[f]
    au = next(i for i in range(3) if U[i])
    av = next(i for i in range(3) if V[i])
    su, sv = 2.0 * half[au], 2.0 * half[av]
    nu = max(1, int(round(su / tu))) if su >= tu else 1
    nv = max(1, int(round(sv / tv))) if sv >= tv else 1
    fu = su / tu if su < tu else 1.0
    fv = sv / tv if sv < tv else 1.0

    for i in range(nu):
        for k in range(nv):
            sc, sh = list(center), list(half)
            sc[au] = center[au] - half[au] + su * (i + 0.5) / nu
            sh[au] = su * 0.5 / nu
            sc[av] = center[av] - half[av] + sv * (k + 0.5) / nv
            sh[av] = sv * 0.5 / nv
            t0 = pick(i, k) if pick else (0.0, 0.0, 1.0, 1.0)
            uv = (t0[0], t0[1], t0[0] + fu * (t0[2] - t0[0]), t0[1] + fv * (t0[3] - t0[1]))
            emit_face(verts, indices, f, tuple(sc), tuple(sh), uv)


def emit_box_panels(verts, indices, center, half, skip=(3,)):
    """Коробка, все грани которой порезаны на панели, — для элементов СТЕНЫ (парапет).
    Элементы крыши остаются на emit_box: у пропа текстура ложится на грань один раз."""
    for f in range(6):
        if f not in skip:
            emit_face_panels(verts, indices, f, center, half)


def emit_slab(mesh, seg, cells, gx, gz, cap_bottom, cap_top, seed):
    """Участок стены = коробка блока с посторонним выносом на внешних сторонах.

    Общая с соседом грань эмитится НЕ целиком и не пропускается целиком: рисуется та её часть,
    которую сосед своим сегментом не закрыл. Совпали габариты (обычный случай) — разность пуста
    и грань уходит вся, как раньше. Разошлись — закрывается ровно щель.

    У сегмента с сеткой (grid) боковые панели — это ОКНА: те же квады, но в своём сабмеше и с
    тайлом вместо полного [0,1]. Крышки окнами не становятся никогда: это горизонтальные полки."""
    x0, x1, z0, z1, cx, cz, e = slab_bounds(seg, cells, gx, gz)
    vspan = (seg["y0"], seg["y1"])
    verts, indices = mesh[seg["slot"]]
    side_v, side_i = mesh["windows"] if seg["grid"] else (verts, indices)

    for f, nb in enumerate(FACE_NEIGHBOUR):
        if nb is None:
            if not (cap_top if f == 2 else cap_bottom):
                continue
            center = ((x0 + x1) * 0.5, (seg["y0"] + seg["y1"]) * 0.5, (z0 + z1) * 0.5)
            half = ((x1 - x0) * 0.5, (seg["y1"] - seg["y0"]) * 0.5, (z1 - z0) * 0.5)
            emit_face_panels(verts, indices, f, center, half)
            continue

        hspan = (z0, z1) if f in (0, 1) else (x0, x1)
        pieces = [hspan]
        nbc = (gx + nb[0], gz + nb[1])
        if nbc in cells:
            nb0, nb1, nb2, nb3, _, _, _ = slab_bounds(seg, cells, *nbc)
            pieces = interval_diff(hspan, (nb2, nb3) if f in (0, 1) else (nb0, nb1))
        for piece in pieces:
            center, half = face_box(f, cx, cz, piece, vspan, 0.0, e[f])
            if seg["grid"]:
                # Ячейка окна берёт аспект ТАЙЛА: лист не квадратный, и квадратная ячейка
                # растянула бы окно. Высота ячейки — этаж, ширина = этаж * аспект тайла.
                emit_face_panels(side_v, side_i, f, center, half,
                                 target=(FLOOR_H * window_tile_aspect(), FLOOR_H),
                                 pick=lambda col, row, _f=f: window_tile(seed, gx, gz, _f, col, row))
            else:
                emit_face_panels(side_v, side_i, f, center, half)


def emit_parapet(verts, indices, seg, cells, gx, gz):
    """Стена крыши: по коробке на каждое ОТКРЫТОЕ ребро блока, снаружи вровень со стеной,
    толщиной внутрь. На углу две перпендикулярные коробки пересекаются — так и надо,
    иначе в углу осталась бы дыра."""
    span = GRID * CELL
    cx = gx * CELL + CELL * 0.5 - span * 0.5
    cz = gz * CELL + CELL * 0.5 - span * 0.5
    for f, nb in enumerate(FACE_NEIGHBOUR):
        if nb is None or (gx + nb[0], gz + nb[1]) in cells:
            continue
        # Пролёт грани — МИРОВОЙ (как того требует face_box): на всю ширину блока, чтобы
        # соседние отрезки парапета сходились на общей границе в непрерывный периметр.
        hspan = (cz - CELL * 0.5, cz + CELL * 0.5) if f in (0, 1) else (cx - CELL * 0.5, cx + CELL * 0.5)
        center, half = face_box(f, cx, cz, hspan, (seg["y0"], seg["y1"]), -PARAPET_T)
        emit_box_panels(verts, indices, center, half)


def build_mesh(cells, segs, roofs, seed=0):
    """Ячейки + вертикальные участки -> буферы ПО СЛОТАМ сабмешей."""
    mesh = {slot: ([], []) for slot in SUBMESH_SLOTS}
    span = GRID * CELL
    slabs = [i for i, sg in enumerate(segs) if sg["kind"] == "slab"]
    roof_y = segs[-1]["y0"]

    for (gx, gz) in sorted(cells):
        cx = gx * CELL + CELL * 0.5 - span * 0.5
        cz = gz * CELL + CELL * 0.5 - span * 0.5

        for i, seg in enumerate(segs):
            if seg["kind"] == "parapet":
                verts, indices = mesh[seg["slot"]]
                emit_parapet(verts, indices, seg, cells, gx, gz)
                continue
            # Крышка нужна там, где сегмент ВЫСТУПАЕТ за соседний по высоте (полка цоколя),
            # и на самых краях: снизу — подошва, сверху — плоскость крыши.
            k = slabs.index(i)
            below = segs[slabs[k - 1]]["extrude"] if k > 0 else None
            above = segs[slabs[k + 1]]["extrude"] if k + 1 < len(slabs) else None
            emit_slab(mesh, seg, cells, gx, gz,
                      cap_bottom=(below is None or seg["extrude"] > below),
                      cap_top=(above is None or seg["extrude"] > above),
                      seed=seed)

        verts, indices = mesh["wall"]
        emit_roof_element(verts, indices, roofs[(gx, gz)], cx, roof_y, cz)
    return mesh


def write_model(stem, mesh):
    """Пишет НЕПУСТЫЕ слоты сабмешами в порядке их номеров.

    Индексы в файле ЛОКАЛЬНЫ для своего сабмеша: движок кладёт vertexOffset записи в
    vertex_offset индирект-команды (IndirectDataModule.cpp:65), и он прибавляется к каждому
    индексу. Рабазировать их на общий буфер нельзя — сместится дважды."""
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    vpath = OUT_DIR / (stem + "_v.bin")
    ipath = OUT_DIR / (stem + "_i.bin")

    # Слот без геометрии всё равно получает ЗАПИСЬ, если ниже него есть занятый: движок берёт
    # материал по номеру сабмеша, и пропуск номера отправил бы всё, что за ним, в
    # "material_index out of range". Пустая запись (0 вершин, 0 индексов) рисует ничто.
    used = [SUBMESH_SLOTS[n] for n in mesh if mesh[n][1]]
    top = max(used) if used else -1
    parts = sorted((SUBMESH_SLOTS[n], n, mesh[n]) for n in mesh if SUBMESH_SLOTS[n] <= top)

    entries, all_verts, all_indices = [], [], []
    for mat_index, name, (verts, indices) in parts:
        entries.append((len(all_verts), len(all_indices), len(verts), len(indices), mat_index))
        all_verts += verts
        all_indices += indices

    with open(vpath, "wb") as fv:
        fv.write(struct.pack("I", len(entries)))
        for e in entries:
            fv.write(struct.pack(SUBMESH_ENTRY, *e))
        for v in all_verts:
            fv.write(struct.pack(VERTEX_STRUCT, *v))
    with open(ipath, "wb") as fi:
        fi.write(struct.pack("<%dI" % len(all_indices), *all_indices))
    return vpath, ipath, parts


def print_footprint(roofs):
    for gz in range(GRID):
        row = ("%9s" % roofs[(gx, gz)] if (gx, gz) in roofs else "%9s" % "." for gx in range(GRID))
        print("   " + "".join(row))


def main():
    rng = random.Random()
    cells = pick_footprint(rng)
    n_floors = assign_heights(rng)
    segs = vertical_segments(n_floors, rng)
    roofs = assign_roofs(cells, rng)
    seed = assign_facade(rng)
    mesh = build_mesh(cells, segs, roofs, seed)
    vpath, ipath, parts = write_model(MODEL_NAME, mesh)

    print("Footprint: %d cells, roof element per cell:" % len(cells))
    print_footprint(roofs)
    print("  height  : %d floors x %.1f = %.1f units" % (n_floors, FLOOR_H, n_floors * FLOOR_H))
    px = jpeg_size(str(Path(__file__).resolve().parents[2] / WINDOW_TEXTURE))
    print("  windows : %s %s, sheet %dx%d, tile aspect %.2f -> cell %.2f x %.2f units"
          % (Path(WINDOW_TEXTURE).name, "%dx%d" % px if px else "(not found)",
             WINDOW_SHEET[0], WINDOW_SHEET[1], window_tile_aspect(),
             FLOOR_H * window_tile_aspect(), FLOOR_H))
    print("  segments:")
    for sg in segs:
        print("    %-8s %-8s y %6.1f .. %6.1f  %s%s"
              % (sg["name"], sg["slot"], sg["y0"], sg["y1"],
                 "%d floors" % sg["floors"] if sg["floors"] else "",
                 "  grid" if sg["grid"] else ""))
    print("  submeshes:")
    for mat_index, name, (verts, indices) in parts:
        print("    [%d] %-8s verts=%-6d indices=%d" % (mat_index, name, len(verts), len(indices)))
    print("  -> %s" % vpath)
    print("  -> %s" % ipath)
    print('  models.json: "vertex": "models/buildings/%s_v.bin", "index": "models/buildings/%s_i.bin"'
          % (MODEL_NAME, MODEL_NAME))


if __name__ == "__main__":
    main()
