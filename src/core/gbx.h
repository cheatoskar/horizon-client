// Reader for the header of .Challenge.Gbx files (TMNF/TMUF/TMN ESWC/Sunrise/Original, GBX v3..v6).
// Only the uncompressed header part is read: header chunks 0x...3002 (times), 0x...3003 (uid, name,
// author, collection), 0x...3005 (XML copy of the same data) and 0x...3007 (JPEG thumbnail, comments). The class id byte order differs
// per game family (0x24003000 in ESWC, 0x03043000 in TMF), so chunks are matched on the low 12 bits.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace lr {

struct GbxMapHeader {
    bool ok = false;
    std::string error;
    int gbxVersion = 0;
    uint32_t classId = 0;
    std::string uid, name /* raw, with $ codes */, author, environment /* "Stadium", ... */, mood, mapType;
    int bronzeMs = 0, silverMs = 0, goldMs = 0, authorMs = 0;  // 0 = not set (map never validated)
    int costCoppers = 0;
    int nbLaps = 0;
    bool timesFromBinary = false;  // times came from the binary chunk (else from the XML)
    bool nameFromBinary = false;   // uid/name/author came from the binary chunk (else from the XML)
    // Embedded thumbnail (chunk 0x...3007), raw JPEG bytes, empty if none. Nadeo stores the image
    // bottom-up: flip it vertically after decoding.
    std::string thumbnailJpeg;
    std::string comments;
};

GbxMapHeader parseChallengeHeader(const unsigned char* data, size_t size);
// path is UTF-8 (plain ANSI paths that are not valid UTF-8 are opened as ANSI).
GbxMapHeader readChallengeHeaderFile(const std::string& path);

// "%24000%5e%5e" -> "$000^^"
std::string percentDecode(const std::string& s);
// Removes $rgb / $w / $o ... formatting; "$$" becomes "$".
std::string stripTmCodes(const std::string& s);

} // namespace lr
