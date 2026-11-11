// Value types shared by the API files: vec2, vec3, vec4 (plain floats, registered as POD value types).
#pragma once

namespace hs {

struct Vec2 {
    float x, y;
};
struct Vec3 {
    float x, y, z;
};
struct Vec4 {
    float x, y, z, w;
};

} // namespace hs
