// .bscotm movie file (JSON, compatible with the Python tool's files).
#pragma once
#include <stdint.h>
#include <string>
#include <vector>
#include "common.h"

typedef std::vector<KeyMask> Frames;    // one key bitmask per frame (see KEYS in common.h)

// A note whose text starts with this character is a bookmark (the editor strips it for display; the file
// stores it as a "bookmark":true field).
static const char kBookmarkFlag = '\x01';

static const int kClockVersion = 2;

struct Movie {
    std::string author, created, prelude_id, prelude_desc;
    // Frame-clock generation the movie was timed against: new movies are 2 (exact frame clock, PR #23); a file
    // without the field loads as 0. Files from the 1/60 s grid builds (PRs #21/#22) ran the music clock too fast,
    // so music-timed events (stage clear) now land later.
    int clock_version = kClockVersion;
    bool MaybeStaleClock() const;     // legacy file whose created_utc falls in the PR #21/#22 window
    Frames frames;
    bool has_seed = false;            // RNG seed = the Unix time the game sees at launch; none = real clock
    uint32_t seed = 0;
    bool controller = false;          // the game sees one virtual controller (the PAD_* keys); off = it sees none
    std::vector<std::string> notes;   // UTF-8 note per frame ("" = none); frames.size() long once loaded
    Frames prelude;     // embedded prelude (optional)

    bool Load(const std::wstring& path, std::string& err);
    bool Save(const std::wstring& path, std::string& err) const;
};
