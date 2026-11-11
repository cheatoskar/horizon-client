// The map's UID without calling game code: the engine keeps id names (CMwId) in one global table. Its
// address is read from the instructions of CMwId::GetString (found by its byte pattern in the exe's code,
// the same pattern in ESWC, Sunrise, TMNF and TMUF); then an id is resolved by plain reads:
//   name = *(T + 4) + *( *(T + (id >> 16) * stride + base) + (id & 0xffff) * 4 )
// with T = *global, stride/base = 8/0x10 (TMNF/TMUF) or 12/0x14 (ESWC/Sunrise). The challenge keeps its
// UID, environment and author as three ids in a row (TMNF/TMUF at +0xdc, verified by the 100 TMX Bingo
// plugin); other exes are scanned for the same triple.
#pragma once

#include <cstdint>
#include <string>

#include "gameread.h"

namespace ck {

struct IdTable {
    uintptr_t global = 0;   // address of the table pointer
    int stride = 0, base = 0;
    bool ok() const { return global != 0; }
};

// Scans [image, image + size) for CMwId::GetString. False when not found.
bool findIdTable(Mem& m, uintptr_t image, size_t size, IdTable* out);
// Decodes the instructions after the pattern (code: at least 0x40 bytes from the pattern's start).
bool decodeIdTable(const unsigned char* code, size_t n, IdTable* out);
// The name of an id ("" none / not a named id).
std::string idName(Mem& m, const IdTable& t, uint32_t id);
bool plausibleUid(const std::string& s);
bool knownEnvironment(const std::string& s);
// The challenge's UID; *offset in: a first guess (-1 none), out: where it was found.
std::string challengeUid(Mem& m, const IdTable& t, uintptr_t challenge, int* offset);
// Size of the loaded image from its PE header (0 if unreadable).
size_t imageSize(Mem& m, uintptr_t image);

} // namespace ck
