#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
scene_gen.py — генератор игровой сцены для SDL_Engine (НОВЫЙ формат сцены-папки).

Сцена теперь — ПАПКА: scene.json (ECS) + рядом манифесты ресурсов
(materials.json / textures.json / models.json / shaders.json). Этот скрипт пишет
ТОЛЬКО scene.json — сами объекты. Материалы/текстуры/модели/шейдеры уже приходят
из инита игры и остальных манифестов папки — скрипт лишь ссылается на них по имени.

Формат scene.json (см. ObjectManager::SaveScene / ComponentSpec::Save):
    { "<архетип>": { "count": N, "entities": [...], "<Компонент>": { "<поле>": [колонка] } } }
Ключ архетипа = ОТСОРТИРОВАННЫЕ по алфавиту имена компонентов через запятую.
Внутри каждый компонент — КОЛОНКИ по полям (SoA-стиль): значение i-й сущности лежит
в i-й позиции каждой колонки. Все кубы делят ОДИН архетип → один компактный блок.

Насыпает NUM_CUBES кубов в ОБЪЁМНЫЙ диск (кольцо) с вырезом по центру:
кубы появляются в 3D-области INNER_RADIUS <= sqrt(x^2 + z^2) <= OUTER_RADIUS,
с разбросом по высоте (ось Y) в пределах DISK_THICKNESS — отсюда «объём».

Каждому кубу случайно назначается материал (из CUBE_MATERIALS) и модель
"cube_0".."cube_(N-1)". Модели — процедурные параллелепипеды из Game.cpp
(цикл по kCubeVariants). NUM_CUBE_MODELS ниже ОБЯЗАН совпадать с kCubeVariants.

Запуск (без параметров):
    python scene_gen.py      (или: py scene_gen.py)

Результат пишется СРАЗУ в папку сцены игры: src/game/saved_scene/scene.json
(движок грузит папку "saved_scene" из рабочей папки src/game, см. Game::MainInit →
ctx->LoadScene("main_menu", "saved_scene")). Прочие манифесты в папке не трогаются.
"""

import os
import math
import random

# ============================================================================
#  ПАРАМЕТРЫ ГЕНЕРАЦИИ  (правь здесь)
# ============================================================================
NUM_CUBES = 1000_000          # сколько кубов сгенерировать

# --- Объёмный диск (кольцо) ---
INNER_RADIUS = 50.0         # РАДИУС ВЫРЕЗА по центру: ближе к оси кубов нет
OUTER_RADIUS = 350.0        # МАКСИМАЛЬНЫЙ радиус диска
DISK_THICKNESS = 3.0        # «объём» — полная высота кольца по оси Y (кубы в +/- THICKNESS/2)

# --- Масштаб кубов ---
CUBE_SCALE_MIN = 0.3
CUBE_SCALE_MAX = 0.9

# --- Орбитальные скорости (круговая орбита вокруг центра сцены (0,0) в плоскости XZ) ---
# Предполагаем в центре гравитационный объект. Скорость круговой орбиты: v = sqrt(G*M / r),
# где r — расстояние в плоскости XZ. Настоящая G = 6.674e-11 не нужна: её степень «съедена»
# массой (работаем сразу с произведением GM), иначе для нормальных скоростей масса была бы
# астрономической. Крути CENTRAL_MASS (или GRAVITY_CONST), чтобы менять темп вращения.
# GM ОБЯЗАН совпадать с kGravGM в Game.cpp::SimulateGravity, иначе орбиты «поедут».
GRAVITY_CONST = 1.0         # G без крошечной степени
CENTRAL_MASS = 5000.0       # масса гравитационного объекта в (0,0)
GM = GRAVITY_CONST * CENTRAL_MASS   # μ — стандартный гравитационный параметр
ORBIT_SPEED_SPREAD = 0.35   # индивидуальный разброс скорости, доля (±доля); 0 = идеальные круги

# Фиксированный сид → одна и та же сцена при каждом запуске (None = каждый раз новая).
RANDOM_SEED = 42

# Материалы (уже зарегистрированы в Game.cpp / materials.json) — раздаются кубам случайно.
CUBE_MATERIALS = ["m_orange", "m_gray", "metal1", "metal2", "emission"]

# ----------------------------------------------------------------------------
#  Модели кубов — процедурные параллелепипеды из Game.cpp с именами cube_0..cube_(N-1).
#  Питон только раздаёт эти имена в поле Model.name сцены; сама геометрия строится в игре.
#  NUM_CUBE_MODELS ДОЛЖЕН быть равен kCubeVariants в Game.cpp — иначе имена не сойдутся
#  (движок не найдёт модель по имени и сущность не отрисуется).
# ----------------------------------------------------------------------------
NUM_CUBE_MODELS = 12
CUBE_MODELS = ["cube_{}".format(i) for i in range(NUM_CUBE_MODELS)]

# --- Направленный свет сцены (entity 0) ---
# Освещает весь диск; ortho-область теней = радиус диска. Без него сцена тёмная.
# Ставим отдельной сущностью прямо в scene.json (в ините игры свет не создаётся).
EMIT_DIRECT_LIGHT = True
DIRECT_LIGHT_DIR = (0.0, -1.0, -0.7)
DIRECT_LIGHT_COLOR = (1.0, 1.0, 1.0)
DIRECT_LIGHT_POWER = 2.5
LIGHT_CASCADE_COUNT = 3
LIGHT_CASCADE_RATIO = 3.0

# Папка сцены игры (относительно скрипта) и имя ECS-файла внутри неё.
SCENE_DIR = os.path.join("..", "..", "src", "game", "saved_scene")
OUTPUT_NAME = "scene.json"

# Имена 16 колонок Transform (row-major 4x4; трансляция в w/d/h — индексы 3/7/11).
# Порядок = порядок полей в ComponentSerializer.cpp (реестр Transform).
TRANSFORM_COLS = ["x", "y", "z", "w", "a", "b", "c", "d",
                  "e", "f", "g", "h", "i", "j", "k", "l"]


# ============================================================================
#  Математика матриц (движок ждёт 4x4 row-major, трансляция в индексах 3/7/11)
# ============================================================================
def _rot_x(t):
    c, s = math.cos(t), math.sin(t)
    return [[1, 0, 0], [0, c, -s], [0, s, c]]


def _rot_y(t):
    c, s = math.cos(t), math.sin(t)
    return [[c, 0, s], [0, 1, 0], [-s, 0, c]]


def _rot_z(t):
    c, s = math.cos(t), math.sin(t)
    return [[c, -s, 0], [s, c, 0], [0, 0, 1]]


def _mul3(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def make_transform(pos, rot3x3, scale):
    """Собирает 16 float (row-major) из позиции, поворота 3x3 и равномерного масштаба."""
    x, y, z = pos
    m = [[rot3x3[r][c] * scale for c in range(3)] for r in range(3)]
    return [
        m[0][0], m[0][1], m[0][2], x,
        m[1][0], m[1][1], m[1][2], y,
        m[2][0], m[2][1], m[2][2], z,
        0.0,     0.0,     0.0,     1.0,
    ]


def _fmt(v):
    # %.7g — точность float32 (как SaveTransform движка) и компактный, но валидный JSON-номер.
    return format(v, ".7g")


# ============================================================================
#  Генерация точек / поворотов / скоростей
# ============================================================================
def sample_point():
    """Случайная точка в объёмном кольце. Радиус берётся по sqrt → равномерно по площади."""
    u = random.random()
    r = math.sqrt(u * (OUTER_RADIUS ** 2 - INNER_RADIUS ** 2) + INNER_RADIUS ** 2)
    theta = random.uniform(0.0, 2.0 * math.pi)
    x = r * math.cos(theta)
    z = r * math.sin(theta)
    y = random.uniform(-DISK_THICKNESS * 0.5, DISK_THICKNESS * 0.5)
    return (x, y, z)


def random_rotation():
    """Полностью случайная ориентация (Rz * Ry * Rx) — кубы «плавают» в объёме."""
    rx = _rot_x(random.uniform(0.0, 2.0 * math.pi))
    ry = _rot_y(random.uniform(0.0, 2.0 * math.pi))
    rz = _rot_z(random.uniform(0.0, 2.0 * math.pi))
    return _mul3(_mul3(rz, ry), rx)


def orbital_velocity(pos):
    """Вектор скорости для круговой орбиты вокруг (0,0) в плоскости XZ.

    Скорость перпендикулярна радиусу от центра до объекта, модуль = sqrt(GM / r_xz)
    с индивидуальным разбросом ±ORBIT_SPEED_SPREAD. Компонента по Y нулевая (движение в XZ).
    """
    x, _, z = pos
    r_xz = math.hypot(x, z)
    if r_xz < 1e-6:
        return (0.0, 0.0, 0.0)
    v = math.sqrt(GM / r_xz)
    v *= 1.0 + random.uniform(-ORBIT_SPEED_SPREAD, ORBIT_SPEED_SPREAD)
    # Перпендикуляр к радиусу (x,z) в плоскости XZ — касательная к окружности (единый обход).
    vx = -z / r_xz * v
    vz = x / r_xz * v
    return (vx, 0.0, vz)


# ============================================================================
#  Сериализация scene.json (архетип-колоночный формат)
# ============================================================================
def _num_col(key, values):
    """'"key":[v0,v1,...]' — числовая колонка из уже отформатированных строк."""
    return '"{}":[{}]'.format(key, ",".join(values))


def _light_block():
    """Блок архетипа DirectLight,ShadowCaster (одна сущность, id 0)."""
    dx, dy, dz = DIRECT_LIGHT_DIR
    r, g, b = DIRECT_LIGHT_COLOR
    he = _fmt(OUTER_RADIUS)   # half_extent / half_depth ortho-теней = радиус диска
    dl = ",".join([
        _num_col("dir_x", [_fmt(dx)]), _num_col("dir_y", [_fmt(dy)]), _num_col("dir_z", [_fmt(dz)]),
        _num_col("r", [_fmt(r)]), _num_col("g", [_fmt(g)]), _num_col("b", [_fmt(b)]),
        _num_col("power", [_fmt(DIRECT_LIGHT_POWER)]),
        _num_col("center_x", ["0"]), _num_col("center_y", ["0"]), _num_col("center_z", ["0"]),
        _num_col("half_extent", [he]), _num_col("half_depth", [he]),
        _num_col("cascade_count", [str(int(LIGHT_CASCADE_COUNT))]),
        _num_col("cascade_ratio", [_fmt(LIGHT_CASCADE_RATIO)]),
    ])
    return ('"DirectLight,ShadowCaster":{'
            '"count":1,"entities":[0],'
            '"DirectLight":{' + dl + '},'
            '"ShadowCaster":{}}')


def build_scene():
    if INNER_RADIUS < 0 or OUTER_RADIUS <= INNER_RADIUS:
        raise ValueError("Нужно 0 <= INNER_RADIUS < OUTER_RADIUS")

    if RANDOM_SEED is not None:
        random.seed(RANDOM_SEED)

    # Свет — entity 0, кубы — 1..NUM_CUBES (файл-локальные id уникальны по всему файлу).
    first_cube_id = 1 if EMIT_DIRECT_LIGHT else 0

    # Колонки-аккумуляторы кубов (SoA): 16 Transform + Model + Material + 3 Velocity.
    t_cols = [[] for _ in range(16)]
    col_model = []
    col_mat = []
    vx_col, vy_col, vz_col = [], [], []
    used_models = set()

    for _ in range(NUM_CUBES):
        pos = sample_point()
        rot = random_rotation()
        scale = random.uniform(CUBE_SCALE_MIN, CUBE_SCALE_MAX)
        transform = make_transform(pos, rot, scale)
        for k in range(16):
            t_cols[k].append(_fmt(transform[k]))

        name = random.choice(CUBE_MODELS)
        used_models.add(name)
        col_model.append('"{}"'.format(name))

        material = random.choice(CUBE_MATERIALS)
        col_mat.append('["{}"]'.format(material))   # jagged: один материал на куб

        vx, vy, vz = orbital_velocity(pos)
        vx_col.append(_fmt(vx))
        vy_col.append(_fmt(vy))
        vz_col.append(_fmt(vz))

    n = NUM_CUBES
    ids = ",".join(str(first_cube_id + i) for i in range(n))

    transform_obj = ",".join(_num_col(TRANSFORM_COLS[k], t_cols[k]) for k in range(16))
    model_obj = _num_col("name", col_model)          # строки уже с кавычками
    material_obj = '"names":[{}]'.format(",".join(col_mat))
    velocity_obj = ",".join([_num_col("x", vx_col), _num_col("y", vy_col), _num_col("z", vz_col)])
    # Draw: все кубы видимы, alpha=1, flags=0.
    draw_obj = ",".join([
        _num_col("visible", ["true"] * n),
        _num_col("alpha", ["1"] * n),
        _num_col("flags", ["0"] * n),
    ])

    cubes_block = ('"Draw,Material,Model,Shadow,Transform,Velocity":{'
                   '"count":' + str(n) + ','
                   '"entities":[' + ids + '],'
                   '"Transform":{' + transform_obj + '},'
                   '"Model":{' + model_obj + '},'
                   '"Material":{' + material_obj + '},'
                   '"Velocity":{' + velocity_obj + '},'
                   '"Shadow":{},'
                   '"Draw":{' + draw_obj + '}}')

    blocks = []
    if EMIT_DIRECT_LIGHT:
        blocks.append(_light_block())
    blocks.append(cubes_block)

    text = "{\n" + ",\n".join(blocks) + "\n}\n"
    return text, used_models


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    out_dir = os.path.join(script_dir, SCENE_DIR)
    out_path = os.path.join(out_dir, OUTPUT_NAME)

    if not os.path.isdir(out_dir):
        raise SystemExit("Папки сцены нет: {}\n(ожидается src/game/saved_scene с манифестами ресурсов)".format(out_dir))

    text, used_models = build_scene()

    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)

    print("OK: {} кубов -> {}".format(NUM_CUBES, out_path))
    print("Кольцо: R_внутр={}  R_внеш={}  толщина={}".format(
        INNER_RADIUS, OUTER_RADIUS, DISK_THICKNESS))
    print("Свет (entity 0): {}".format("да" if EMIT_DIRECT_LIGHT else "нет"))
    print("Использовано моделей: {} из {} (cube_0..cube_{}).".format(
        len(used_models), NUM_CUBE_MODELS, NUM_CUBE_MODELS - 1))
    print("Проверь: kCubeVariants в Game.cpp == NUM_CUBE_MODELS ({}), "
          "kGravGM == GM ({}).".format(NUM_CUBE_MODELS, GM))


if __name__ == "__main__":
    main()
