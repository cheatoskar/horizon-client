// Member types of the engine reflection and how their values are shown. Pure functions: no memory reads.
//
// SMwParamInfo.type numbers come in families of base / Buffer / BufferCat / Array (+ Range for the
// numeric ones). Forever (TMNF/TMUF) inserted an Id family at 27, so every number from 27 on is 4
// higher than in ESWC/Sunrise. Worked out from the name lists in tm-modding-knowledge
// data/reflection/<exe>_classes.json (e.g. ESWC 32 / TMNF 36 hold the float members, 49 / 53 the vec3).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace nx {

enum class Engine { Legacy, Forever }; // Legacy: ESWC, Sunrise; Forever: TMNF, TMUF

enum class Kind : uint8_t {
    Unknown, Proc, ProcEx, Bool, Nod, Color, Enum, Int, Iso4, Iso3, Id, Natural, Real, String, WString, Vec2, Vec3, Vec4, Int3
};
enum class Shape : uint8_t { Single, Buffer, BufferCat, Array, Range };

struct TypeInfo {
    Kind kind = Kind::Unknown;
    Shape shape = Shape::Single;
};

TypeInfo classify(uint32_t type, Engine engine);
const char* kindName(Kind k);                  // "float", "nod", ...
std::string typeLabel(TypeInfo t, const char* nodClass); // "float", "CGameCtnChallenge", "uint list", "nod list"
size_t elemSize(Kind k);                       // bytes of one value in memory (0: not a value)
bool isMethod(Kind k);
bool isList(Shape s);                          // Buffer / BufferCat ({count, items, capacity})

// ---- formatting (all ASCII/UTF-8, short) ----
std::string fmtFloat(float v);                 // "1.5", "-0.25", "1e+06", "nan"
std::string fmtFloats(const float* v, int n);  // "1, 2.5, -3"
std::string fmtBool(uint32_t v);               // "true", "false", "7 (bool?)"
std::string fmtInt(int32_t v);
std::string fmtUint(uint32_t v);
std::string fmtHex(uint32_t v);                // "0x0000002A"
std::string fmtPtr(uintptr_t p);               // "0x0A1B2C30", "null"
// Quoted text: control characters escaped, cut after max bytes with "...".
std::string fmtText(const char* s, size_t n, size_t max);
// UTF-16 to quoted UTF-8 text (invalid units become '?').
std::string fmtWText(const wchar_t* s, size_t n, size_t max);
// A fixed-size value of kind k from raw memory (n = elemSize(k)). Iso4: "pos x, y, z"; Color: "r, g, b".
std::string fmtValue(Kind k, const void* raw, size_t n);
// Longer form for tooltips (full matrices); "" when fmtValue already says everything.
std::string fmtValueLong(Kind k, const void* raw, size_t n);

} // namespace nx
