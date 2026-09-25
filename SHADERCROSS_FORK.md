# Форк SDL_shadercross — что лежит и как обновлять

`external/SDL3_shadercross` — исходники upstream `libsdl-org/SDL_shadercross`, вендоренные в репо
вместо прежнего бинарного дистрибутива. Зачем: shadercross вызывает DXC без `-fspv-target-env` и
собирает только SPIR-V 1.0, а движок официально работает под Vulkan 1.3. Свойства для версии в
upstream нет, поэтому нужна своя правка.

Правки помечены тем же тегом, что и в SDL:

```
grep -rn "ENGINE-FORK" external/SDL3_shadercross/
```

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

Не правка, но рядом: `export(TARGETS SDL3_shadercross-static)` требует, чтобы SPIRV-Cross тоже
был в export set. Это делает корневой `CMakeLists.txt`, как и vendored-режим самого shadercross.

## Обновление

1. `git archive` нового upstream поверх `external/SDL3_shadercross` (кроме `external/`).
2. Коммит сабмодуля SPIRV-Cross — из `git submodule status` нового upstream; оттуда же
   `git archive` исходников библиотеки (список путей — как в таблице выше).
3. DXC — новый релиз с `github.com/microsoft/DirectXShaderCompiler/releases`, те же каталоги.
4. Приложить правки по тегу `ENGINE-FORK`, обновить таблицу версий.
5. **Кэш шейдеров.** В его ключе сейчас версия shadercross (`SDL_SHADERCROSS_*_VERSION`), а не
   DXC. Смена одного DXC ключ не меняет — после обновления DXC кэш чистить руками, пока ключ не
   переведён на версию DXC.

Upstream запрещает контрибуции с кодом от ИИ (`CLAUDE.md`, `AGENTS.md` в каталоге), так что
правки отсюда в upstream в таком виде не отправлять.
