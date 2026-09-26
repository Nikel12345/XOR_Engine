# Форк SDL_shadercross — что лежит и как обновлять

`external/SDL3_shadercross` — исходники upstream `libsdl-org/SDL_shadercross`, вендоренные в репо
вместо прежнего бинарного дистрибутива. Зачем: shadercross вызывает DXC без `-fspv-target-env` и
собирает только SPIR-V 1.0, а движок официально работает под Vulkan 1.3. Свойства для версии в
upstream нет, поэтому нужна своя правка.

Правки помечены тем же тегом, что и в SDL:

```
grep -rn "ENGINE-FORK" external/SDL3_shadercross/
```

## Публичный форк

`github.com/Nikel12345/SDL_shadercross`, ветка `main`: upstream `1ff05bec` плюс три коммита
(правки 2–4 ниже, по одной на коммит). Комментарии в правках по-английски — по той же причине,
что у SDL: вендоренная копия и форк держатся одинаковыми, а две языковые версии одного патча
не поддерживаются.

В форк не входят правка 1 (`.gitignore`) и содержимое `external/`: там это сабмодули и
скачиваемые бинарники DXC, как в upstream. Поэтому синхронность проверяется только по коду:

```
diff -rq --strip-trailing-cr -x external -x .gitignore -x .git <клон форка> external/SDL3_shadercross
```

Пусто — синхронны. Правка вендоренной копии без того же коммита в форке эту проверку ломает.

## Что откуда

| путь | источник | версия |
|---|---|---|
| `external/SDL3_shadercross/` | `github.com/libsdl-org/SDL_shadercross`, `git archive` | `1ff05bec` (2026-09-03), 3.0.0 |
| `…/external/SPIRV-Cross/` | `github.com/KhronosGroup/SPIRV-Cross`, коммит сабмодуля shadercross | `1a616956` |
| `…/external/DirectXShaderCompiler-binaries/` | релиз Microsoft `dxc_2026_07_29.zip` | v1.9.2607 |

- **SPIRV-Cross** — только исходники библиотеки: `reference/`, `shaders*/`, тесты и примеры не
  взяты. Часть тестовых файлов не распаковывается на Windows из-за длины пути, а сборке они не
  нужны.
- **DXC** — готовые бинарники, только x64: `bin/x64` (dxcompiler.dll, dxil.dll), `lib/x64`,
  `inc/`, лицензии. Из исходников не собирается: это LLVM, сборка на десятки минут ради
  библиотеки, которую мы не правим. Раскладка каталога — та, которую ищет штатный
  `cmake/FindDirectXShaderCompiler.cmake`.
- Сабмодули `SPIRV-Headers`, `SPIRV-Tools`, `DirectXShaderCompiler` не взяты: они нужны только
  для сборки DXC из исходников (`SDLSHADERCROSS_VENDORED=ON`).

## Как собирается

Корневой `CMakeLists.txt`: сначала `add_subdirectory` SPIRV-Cross (статически), затем shadercross
с `SDLSHADERCROSS_VENDORED=OFF`, `STATIC=ON`, без CLI/тестов/install. Движок линкует
`SDL3_shadercross::SDL3_shadercross-static` в `EngineGpu`; отдельной `SDL3_shadercross.dll`
больше нет. `copy_runtime_dlls` кладёт `dxcompiler.dll` и `dxil.dll` рядом с exe.

**Почему DXC обязан лежать рядом с exe.** Windows ищет DLL сначала в каталоге exe, потом по
PATH. До вендоринга рядом с exe DXC не было, и все шейдеры молча компилировал DXC из Vulkan SDK
(`G:\Vulkan SDK\Bin`, 1.8), а не тот, что лежал в дистрибутиве shadercross. На машине без SDK
промах кэша шейдеров был бы ошибкой компиляции. Проверка: зонд
`src/sandbox/src/ShadercrossBuildProbe.cpp` печатает, откуда загружен `dxcompiler.dll`.

## Правки

1. **`.gitignore`** — upstream игнорирует `external/DirectXShaderCompiler-binaries` (их там
   скачивают при каждом checkout). У нас они в репо, строка закомментирована.
2. **`CMakeLists.txt`, поиск SPIRV-Cross** — проверка `if(NOT TARGET spirv_cross_c)` ждёт имя
   из пакета find_package. SPIRV-Cross, добавленный через `add_subdirectory`, называет цель
   `spirv-cross-c`, и без правки shadercross снова звал бы find_package и падал.

3. **Свойство `SDL_SHADERCROSS_PROP_SPIRV_TARGET_ENV_STRING`** (`SDL_shadercross.h`,
   `SDL_ShaderCross_INTERNAL_CompileUsingDXC`) — передаётся в DXC как
   `-fspv-target-env=<значение>`. Без свойства аргументы совпадают с upstream. Массив аргументов
   там фиксированного размера, под новый аргумент он увеличен на один слот. Свойство действует
   и на путь в DXIL: `CompileDXILFromHLSL` идёт через SPIR-V с теми же `props`.
4. **`SDL_ShaderCross_GetDXCVersion(major, minor, commit_count)`** — версия загруженного
   `dxcompiler` через `IDxcVersionInfo2`. `commit_count` различает релизы с одинаковым
   `major.minor` (1.9.2602 и 1.9.2607 оба отвечают 1.9). Добавлена и в `SDL_shadercross.sym`.

Движок: версия Vulkan задаётся в `EngineConfig` (`vulkan_major`/`vulkan_minor`), из неё
`Engine::InitPlatform` берёт и `apiVersion` девайса, и строку `vulkanX.Y` для `ShaderManager`.
В ключ кэша шейдеров идут версия DXC и target-env — смена любого из них сама сбрасывает кэш.

Проверено зондом `src/sandbox/src/ShadercrossBuildProbe.cpp`: шейдер с `WaveActiveCountBits`
без свойства падает («Vulkan 1.1 is required for Wave Operation»), с `vulkan1.3` собирается в
SPIR-V 1.6; все 36 вариантов шейдеров движка (включая `surface.hlsl` с дефайнами
`DefaultShaderSet`) под `vulkan1.0` и `vulkan1.3` дают одинаковую рефлексию.

Не правка, но рядом: `export(TARGETS SDL3_shadercross-static)` требует, чтобы SPIRV-Cross тоже
был в export set. Это делает корневой `CMakeLists.txt`, как и vendored-режим самого shadercross.

## Обновление

1. В форке: `git rebase` ветки `main` на новый upstream, прогнать сборку, `git push --force`.
2. `git archive main` форка поверх `external/SDL3_shadercross` (кроме `external/`), затем снова
   закомментировать строку в `.gitignore` (правка 1).
3. Коммит сабмодуля SPIRV-Cross — из `git submodule status` нового upstream; оттуда же
   `git archive` исходников библиотеки (список путей — как в таблице выше).
4. DXC — новый релиз с `github.com/microsoft/DirectXShaderCompiler/releases`, те же каталоги.
5. Обновить таблицу версий и базовый коммит в разделе «Публичный форк», проверить синхронность.
6. Кэш шейдеров чистить не нужно: версия DXC в его ключе.

Upstream запрещает контрибуции с кодом от ИИ (`CLAUDE.md`, `AGENTS.md` в каталоге), так что
правки отсюда в upstream в таком виде не отправлять.
