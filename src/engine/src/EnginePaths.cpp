#include "EnginePaths.h"

std::string EnginePath(std::string_view rel)
{
    std::string path = XOR_ENGINE_ASSET_DIR;
    path += '/';
    path += rel;
    return path;
}
