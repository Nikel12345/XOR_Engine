#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
scene_gen.py — генератор игровой сцены для SDL_Engine.

Насыпает NUM_CUBES кубов в ОБЪЁМНЫЙ диск (кольцо) с вырезом по центру:
кубы появляются в 3D-области INNER_RADIUS <= sqrt(x^2 + z^2) <= OUTER_RADIUS,
с разбросом по высоте (ось Y) в пределах DISK_THICKNESS — отсюда «объём».

Каждому кубу случайно назначается материал и одна из моделей "cube_0".."cube_(N-1)".
Сами модели — процедурные параллелепипеды, которые генерирует Game.cpp
(цикл по kCubeVariants). NUM_CUBE_MODELS ниже ОБЯЗАН совпадать с kCubeVariants.

Запуск (без параметров):
    python scene_gen.py

Результат: saved_scene.scene создаётся РЯДОМ с этим скриптом.
Чтобы игра его подхватила — скопируй в src/game/saved_scene.scene
(движок грузит "saved_scene.scene" из рабочей папки src/game, см. Game::MainInit).
"""

import os
import math
import random

# ============================================================================
#  ПАРАМЕТРЫ ГЕНЕРАЦИИ  (правь здесь)
# ============================================================================
NUM_CUBES = 10_000            # сколько кубов сгенерировать

# --- Объёмный диск (кольцо) ---
INNER_RADIUS = 50.0         # РАДИУС ВЫРЕЗА по центру: ближе к оси кубов нет
OUTER_RADIUS = 350.0        # МАКСИМАЛЬНЫЙ радиус диска
DISK_THICKNESS = 3.0       # «объём» — полная высота кольца по оси Y (кубы в +/- THICKNESS/2)

# --- Масштаб кубов ---
CUBE_SCALE_MIN = 0.3
CUBE_SCALE_MAX = 0.9

# --- Орбитальные скорости (круговая орбита вокруг центра сцены (0,0) в плоскости XZ) ---
# Предполагаем в центре гравитационный объект. Скорость круговой орбиты: v = sqrt(G*M / r),
# где r — расстояние в плоскости XZ. Настоящая G = 6.674e-11 не нужна: её степень «съедена»
# массой (работаем сразу с произведением GM), иначе для нормальных скоростей масса была бы
# астрономической. Крути CENTRAL_MASS (или GRAVITY_CONST), чтобы менять темп вращения.
GRAVITY_CONST = 1.0          # G без крошечной степени
CENTRAL_MASS = 5000.0          # масса гравитационного объекта в (0,0)
GM = GRAVITY_CONST * CENTRAL_MASS   # μ — стандартный гравитационный параметр
ORBIT_SPEED_SPREAD = 0.35    # индивидуальный разброс скорости, доля (±5%); 0 = идеальные круги

# Фиксированный сид → одна и та же сцена при каждом запуске (None = каждый раз новая).
RANDOM_SEED = 42

# Материалы (уже зарегистрированы в Game.cpp) — раздаются кубам случайно.
CUBE_MATERIALS = ["m_orange", "m_gray", "metal1", "metal2", "emission"]

# ----------------------------------------------------------------------------
#  Модели кубов — процедурные параллелепипеды из Game.cpp с именами cube_0..cube_(N-1).
#  Питон только раздаёт эти имена в поле "Model =" сцены; сама геометрия строится в игре.
#  NUM_CUBE_MODELS ДОЛЖЕН быть равен kCubeVariants в Game.cpp — иначе имена не сойдутся
#  (движок не найдёт модель по имени и сущность не отрисуется).
# ----------------------------------------------------------------------------
NUM_CUBE_MODELS = 12
CUBE_MODELS = ["cube_{}".format(i) for i in range(NUM_CUBE_MODELS)]

# Направленный свет сцены (освещает весь диск + область теней под его размер).
DIRECT_LIGHT_POWER = 2.5

OUTPUT_NAME = "saved_scene.scene"


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
    # %.7g — тот же формат, что SaveTransform в движке (ComponentSerializer.cpp).
    return format(v, ".7g")


# ============================================================================
#  Генерация
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


def build_scene():
    if INNER_RADIUS < 0 or OUTER_RADIUS <= INNER_RADIUS:
        raise ValueError("Нужно 0 <= INNER_RADIUS < OUTER_RADIUS")

    if RANDOM_SEED is not None:
        random.seed(RANDOM_SEED)

    lines = []

    # --- entity 0: направленный свет + кастер теней, площадь теней под размер диска ---
    he = _fmt(OUTER_RADIUS)  # half_extent / half_depth ортопроекции теней = радиус диска
    # lines.append("[entity] 0")
    # lines.append("  ShadowCaster =")
    # lines.append("  DirectLight = 0 -1 -0.7 1 1 1 {p} 0 0 0 {he} {he} 3 3".format(
    #     p=_fmt(DIRECT_LIGHT_POWER), he=he))

    used_models = set()

    # --- кубы (entity 1..NUM_CUBES) ---
    for eid in range(1, NUM_CUBES + 1):
        pos = sample_point()
        rot = random_rotation()
        scale = random.uniform(CUBE_SCALE_MIN, CUBE_SCALE_MAX)
        transform = make_transform(pos, rot, scale)

        name = random.choice(CUBE_MODELS)
        used_models.add(name)
        material = random.choice(CUBE_MATERIALS)
        vel = orbital_velocity(pos)

        lines.append("[entity] {}".format(eid))
        lines.append("  Material = {}".format(material))
        lines.append("  Transform = {}".format(" ".join(_fmt(v) for v in transform)))
        lines.append("  Model = {}".format(name))
        lines.append("  Velocity = {}".format(" ".join(_fmt(c) for c in vel)))
        lines.append("  Shadow =")
        lines.append("  Draw = 1 1 0")

    return "\n".join(lines) + "\n", used_models


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    scene_folder = "..\\..\\src\\game\\"
    out_path = os.path.join(script_dir, scene_folder, OUTPUT_NAME)

    text, used_models = build_scene()

    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)

    print("OK: {} кубов -> {}".format(NUM_CUBES, out_path))
    print("Кольцо: R_внутр={}  R_внеш={}  толщина={}".format(
        INNER_RADIUS, OUTER_RADIUS, DISK_THICKNESS))
    print("Использовано моделей: {} из {} (cube_0..cube_{}).".format(
        len(used_models), NUM_CUBE_MODELS, NUM_CUBE_MODELS - 1))
    print("Проверь, что kCubeVariants в Game.cpp == NUM_CUBE_MODELS ({}).".format(NUM_CUBE_MODELS))


if __name__ == "__main__":
    main()
