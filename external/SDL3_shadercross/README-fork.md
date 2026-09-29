# Fork changes

This fork adds three small changes on top of upstream `1ff05bec`, one commit each. They exist so
that an engine can build shadercross from source inside its own CMake tree and compile SPIR-V for
a Vulkan version newer than 1.0.

## SPIRV-Cross added by the parent project

`CMakeLists.txt` looked for SPIRV-Cross only under the target name that its package config
exports, `spirv_cross_c`. A parent project that adds SPIRV-Cross with `add_subdirectory` gets the
target `spirv-cross-c` instead, and shadercross then fell back to `find_package` and failed. Both
names are now accepted.

## `SDL_SHADERCROSS_PROP_SPIRV_TARGET_ENV_STRING`

Upstream calls DXC without `-fspv-target-env`, so every shader is compiled as SPIR-V 1.0 for
Vulkan 1.0. Wave intrinsics such as `WaveActiveCountBits` are then rejected with "Vulkan 1.1 is
required for Wave Operation".

The new property on the `props` of `SDL_ShaderCross_HLSL_Info` is passed to DXC as
`-fspv-target-env=<value>`, for example `vulkan1.3`. Without it the arguments are exactly
upstream's. It also affects the DXIL path: unless `SDL_SHADERCROSS_PROP_HLSL_SKIP_SPIRV_ROUNDTRIP_BOOLEAN`
is set, `SDL_ShaderCross_CompileDXILFromHLSL` goes through SPIR-V with the same `props`.

## `SDL_ShaderCross_GetDXCVersion`

```c
bool SDL_ShaderCross_GetDXCVersion(Uint32 *major, Uint32 *minor, Uint32 *commit_count);
```

Returns the version of the `dxcompiler` library that is actually loaded, via `IDxcVersionInfo2`.
`commit_count` tells apart releases that share `major.minor`: 1.9.2602 and 1.9.2607 both report
1.9. A caller that caches compiled shaders can put this version into the cache key, so that
switching DXC invalidates the cache.

The function is exported and listed in `SDL_shadercross.sym`.
