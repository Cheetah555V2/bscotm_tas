// .bscotm movie file (JSON, compatible with the Python tool's files).
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

typedef std::vector<uint16_t> Frames;   // one key bitmask per frame (see KEYS)

// A note whose text starts with this character is a bookmark (the editor strips it for display; the file
// stores it as a "bookmark":true field).
static const char kBookmarkFlag = '\x01';

struct Movie {
    std::string author, created, prelude_id, prelude_desc;
    Frames frames;
    bool has_seed = false;            // RNG seed = the Unix time the game sees at launch; none = real clock
    uint32_t seed = 0;
    std::vector<std::string> notes;   // UTF-8 note per frame ("" = none); frames.size() long once loaded
    Frames prelude;     // embedded prelude (optional)

    bool Load(const std::wstring& path, std::string& err);
    bool Save(const std::wstring& path, std::string& err) const;
};
