#pragma once

#include <cstdint>

enum class DiffuseGiMode : uint32_t { None, DDGI, SSGI, RayTraced };
enum class SpecularGiMode : uint32_t { None, SSR };
enum class GiDisplayMode : uint32_t { Composite, DiffuseOnly, SpecularOnly };

struct RenderOptions {
    DiffuseGiMode diffuse = DiffuseGiMode::DDGI;
    SpecularGiMode specular = SpecularGiMode::None;
    GiDisplayMode display = GiDisplayMode::Composite;
    uint32_t legacyDebugMode = 0;
    bool resetProbeHistory = false;
};
