// .bscotm movie file (JSON, compatible with the Python tool's files).
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

typedef std::vector<uint16_t> Frames;   // one key bitmask per frame (see KEYS)

struct Movie {
    std::string author, created, prelude_id, prelude_desc;
    Frames frames;
    std::vector<std::string> notes;   // UTF-8 note per frame ("" = none); frames.size() long once loaded
    Frames prelude;     // embedded prelude (optional)

    bool Load(const std::wstring& path, std::string& err);
    bool Save(const std::wstring& path, std::string& err) const;
};
