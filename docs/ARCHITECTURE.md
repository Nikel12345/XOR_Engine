# Архитектура XOR Engine

XOR Engine — набор статических библиотек. Приложение линкует их и остаётся хозяином входа: оно
создаёт `SDL_Window` и `SDL_GPUDevice`, настраивает свопчейн и передаёт готовое в
`Engine(window, dev, w, h)`. Дальше цикл кадра ведёт движок, но точка входа, окно и девайс
принадлежат приложению.

Скриптового слоя нет: логика пишется на C++ и линкуется рядом. Расширяется движок не
наследованием, а регистрацией — см. «Швы».

---

## Цели сборки

```
        EngineEcs                       EngineGpu
        ECS-ядро                        GPU-кор
        не знает ни рендера,            не знает про ECS, модели,
        ни GPU                          материалы, ImGui, SDL_image
             ^                               ^
             |                               |
             +----------+---------+----------+
                        |         |
                     Engine     Physics
             рендер, менеджеры,  коллизии и контакты
             data-модули, UI,    на compute
             цикл кадра
                        ^         ^
                        +----+----+
                             |
                        приложение
```

| цель | что внутри | почему отдельно |
|---|---|---|
| **EngineEcs** | `ObjectManager` (+`.inl`), `BaseComponents`, `ComponentSerializer`, `SceneData`, `Aliases.h` | ECS обязан оставаться листом: его линкует и `Engine`, и `Physics`. Тянет только SDL3 и `yyjson` (колоночная сериализация сцены) |
| **EngineGpu** | `QueueManager`, `TransferManager`, `BufferManager` (+`_Binds`/`_Update`/`_Utils`), `ShaderManager` (+`_ShadersCreate`/`_SPVLoad`), `TextureManager`, `PreviewPacker`, `PassManager`, `RenderCommandData`, `GpuTaskContext`, `SparseRankChannel`, `EngineProfiler` | Буферы, шейдеры, текстуры, проходы — без единого знания о сцене и рендер-логике |
| **Engine** | менеджеры, data-модули, `BatchBuilder`, `EngineContext`, дефолт-сеты, UI, цикл кадра | Линкует `EngineGpu` и `EngineEcs` как PUBLIC и склеивает их |
| **Physics** | `PhysicsBufferSet`, `PhysicsComputeSet`, `CollisionShapes`, `ContactSystem`, `DebugColliderSystem` | Линкует **только** `EngineGpu` + `EngineEcs`, без `Engine`, рендера и ImGui. Своего PCH не имеет намеренно — это работающая проверка, что слоение не протекло |

Граница слоёв держится одним требованием: **физике нужны буферы и compute, но не нужен рендер.**
Всё, что ломает возможность собрать `Physics` без `Engine`, ломает слоение.

Отсюда и правило, на котором держится вся раскладка: **менеджеры не держат указателей друг на
друга и не вызывают друг друга напрямую.** Менеджер, владеющий менеджером, немедленно затягивает
свой слой в чужой, и физика перестаёт собираться. Кроссменеджерские операции живут на
`EngineContext`, который передаёт нужные менеджеры **параметрами** в листовой метод.

---

## Внутри `Engine`: роли

Файлы читаются через роли, а не по алфавиту.

- **Менеджеры** (`CameraManager`, `MaterialManager`, `ModelManager`, `PipeManager`, `InputManager`,
  `FontManager`, `TextureLoader`, `GeometryPool`, `RangeAllocator`, `ThreadController`,
  `SlotController`) — каждый владеет реестром своего вида ресурса и **только им**.
- **`EngineContext`** — единственное место кроссменеджерских операций и публичный фасад:
  `CreateMaterial`, `CreateTextureFromFile`, `CreateModel`, `CreateFont`, `CreateGeometryPool`.
  Узкая GPU-половина того же фасада — `GpuTaskContext`; ею пользуются шейдер-сеты и физика.
- **Data-модули** (`*DataModule.cpp`: `Transform`, `Instance`, `PIB`, `Indirect`, `Light`,
  `BoundSphere`, `TextureState`, `UI`) — покадровый мост «ECS → GPU-буфер», по буферу на модуль.
  Место, где сцена превращается в байты.
- **Дефолт-сеты** (`DefaultRenderPassSet`, `DefaultShaderSet`, `DefaultUpdateSet`,
  `DefaultCommandSet`) — **наполнение** движка, а не его ядро: проходы, шейдеры, инструкции
  заливки и команды ввода по умолчанию. Кому нужен свой конвейер — пишет свой сет, ядро при этом
  не открывается.
- **UI** — две независимые вещи под одним префиксом: `UI_ImGui` / `UI_Hierarchy` / `UI_Inspector` /
  `UI_AssetBrowser` / `UI_ComponentEditor` — редактор на ImGui; `UI_Yoga` + `UI_DataModule` —
  UI приложения как ECS-энтити с flex-раскладкой Yoga.

Дальше — `BatchBuilder` (дерево батчей из ECS) и ядро цикла: `Engine.cpp`, `Engine_Frame.cpp`,
`Engine_Scene.cpp`.

Одно место, где имя не совпадает с файлом: живая камера — `class Camera` в
`CameraStruct.h`/`.cpp`, рядом с `CameraManager`.

---

## Швы: как движок расширяется, не открываясь

Единый приём вместо наследования — **регистрация инструкции по имени ресурса**. Кто её
зарегистрировал, движку неизвестно, поэтому своё добавляется снаружи, без правки ядра:

| шов | где | что регистрируют |
|---|---|---|
| заливка буфера | `BufferManager::CreateUpdateInstruction` (+ `PrePass` / `ReadBack` / `PostReadback`) | пара функций «размер» + «залить», опционально смещение |
| пуш/диспатч шейдера | `ShaderManager::CreatePushInstruction<T>` | что положить в push-константы программы с этим именем |
| ресайз таргета | `TextureManager` → `ExecuteResizeInstructions` | как пересоздать атлас под новое разрешение |
| число блоков прохода | `PassManager::CreateRegionCountInstruction` | сколько дроу проход даст за кадр |
| проходы | `PassManager::CreateRenderPass` / `CreateComputePass` / `CreateComputePrepass` / `CreateBlitPass` | тело прохода и его место в порядке |
| команды ввода | `InputManager::RegisterCommand` / `PushCommand` | единственный законный способ мутировать ECS не из sim-потока |

Второй сквозной приём — **ссылки по имени, а не указателем**: `ShaderName`, `MaterialName`,
`AtlasName`, `RenderPassName` (все — `std::string`, см. `Aliases.h`). Имя переживает сериализацию
сцены и перезагрузку реестров; указатель добывается на месте использования. Поле `debug_name` в
этом не участвует — оно подпись для логов и редактора, а не ключ: восстанавливать по нему имя
значит завести вторую систему идентичности, которая разойдётся с реестром.

---

## Потоки и слоты

`ThreadController` поднимает пять потоков: **Simulation** (итерация приложения +
`Engine::PrepareFunc`) → **Upload** → **Compute** → **Render** → **Fence**. Кадры едут по слотам,
`BUFFERING_LEVEL = 3` (`config.h`), раздаёт их `SlotController`; sim и render расцеплены — при
нехватке GPU кадры пропускаются, симуляция не тормозит.

Следствия, нужные сразу:

- **ECS мутирует только sim-поток.** С UI-потока — командой через `InputManager`.
- **Рендер не читает живой ECS** — он читает слепки слота (`RenderSnapshot.h`).
- Панели редактора живой ECS всё же читают, без замков. Это осознанный размен: худшее, что
  может случиться, — рваное значение на один кадр, для редактора безвредное.

Как устроены фазы, слепки и гейты — `docs/render-pipeline/frame.md`.

---

## Внешние зависимости

| что | как подключено | кто линкует |
|---|---|---|
| **SDL3** | **форк**, собирается из исходников `external/SDL3` (правки помечены `ENGINE-FORK`; цель — развести заливку и рендер по разным очередям). Наружу торчит цель `SDL3::SDL3` — она несёт и include-пути, и линк, руками пути прописывать нельзя | PUBLIC у `EngineEcs` / `EngineGpu`, дальше транзитивно |
| **SDL3_image / SDL3_ttf / SDL3_shadercross** | бинарные дистрибутивы; грузят `SDL3.dll` динамически, **ABI привязан к версии форка** — бампаешь SDL, обновляй и их | PRIVATE: image — только `TextureLoader`, ttf — только `FontManager`, shadercross — только `ShaderManager` |
| **ImGui + ImGuizmo** | исходники, свой таргет `ImGui` | PRIVATE к `Engine`; наружу торчит фасад `UI_ImGui` |
| **yoga** | исходники, требует C++20 | PRIVATE к `Engine`; типы спрятаны за pimpl в `UI_Yoga` |
| **yyjson** | один `.c`, вендорный | `EngineEcs` (сцена) и `Engine` |
| **glm** | header-only | PUBLIC — публичные заголовки его раскрывают |
| **rectpack2D** | header-only | PRIVATE, только упаковка атласа в `TextureManager` |

---

## Раскладка на диске

- **CMake запускается только из корня репозитория.** Из подкаталога сборка молча проходит мимо —
  и запускается устаревший exe.
- **Шейдеры движка** — `src/engine/shaders_code/`, по подпапкам проходов (`main_pass`,
  `shadow_pass`, `transparent_pass`, `skybox`, `ui`, `comp`, `debug`). `sparse_rank.hlsli` —
  половина, общая с C++ (`SparseRankChannel.h`): раскладка разреженного канала описана по обе
  стороны границы и обязана совпадать.
- **Кэш SPIR-V** — `shaders/shader_cache` рядом с исполняемым файлом (`SDL_GetBasePath`).
  Ключ кэша включает исходник вместе со всей цепочкой `#include`, поэтому правка `.hlsli`
  инвалидирует зависимые шейдеры сама — чистить руками не нужно.
- **Сцена — это папка**, а не файл: `scene.json` (ECS, колоночно) плюс манифесты ресурсов рядом.
  Точка входа — `Engine::SaveScene` / `LoadScene(имя, папка)`. Порядок загрузки: ресурсы
  (merge-upsert) → ECS (replace) → фикс-ап ссылок → пересборка батчей.
- **Пути ресурсов внутри сцены — относительные** (от корня проекта), абсолютные не хранятся
  никогда: иначе сцена не переживает переезд на другую машину.
- **`scripts/`** — питоновская обвязка вокруг ассетов: `model_loader` (конвертация моделей, свой
  venv), `vec_check.py` (сверка меток `VEC_HOT` с вердиктом векторизатора MSVC, C5001/C5002).
- **`cmake/RuntimeDlls.cmake`** — `copy_runtime_dlls`, раскладка DLL рядом с exe.

В сборке не участвуют `external/SPIRV` (рефлексия ушла в shadercross) и `external/ImGui_old`.

---

## Куда читать дальше

| вопрос | файл |
|---|---|
| Механики, ломающиеся не там, где сделана ошибка | `WARNINGS.md` |
| Что и зачем изменено в SDL | `SDL_FORK.md` |
| Как устроен кадр: слоты, слепки, гейты | `docs/render-pipeline/frame.md` |
| Дерево батчей: группировка, слепок раскладки, вызовы отрисовки | `docs/render-pipeline/batches.md` |
| Индирект, регионы проходов, GPU-каллинг | `docs/render-pipeline/culling.md` |
| Как инстанс находит свои данные: строки, PIB, разреженные каналы | `docs/render-pipeline/instance-data.md` |
| Переключаемые варианты текстур | `docs/render-pipeline/materials.md` |
| Шейдерная программа: реестры, компиляция, пуши | `docs/gpu/shaders/programs.md` |
| Сторона HLSL: слои, регистры, контракты | `docs/gpu/shaders/hlsl.md` |
| Пулы геометрии: потоки, разметка места, путь модели в буферы | `docs/gpu/geometry/pools.md` |
