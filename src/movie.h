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

// A saved version of the movie (Branches window, like TAStudio's branches): its own inputs and notes, the movie row the
// game was at when it was saved, and the settings that change what the game does.
struct Branch {
    std::string name, saved;          // saved: local time, "2026-10-11 14:03:27"
    uint32_t frame = 0;               // the movie row the game ran next when it was saved (the cursor if no game was frozen)
    std::vector<KeyMask> frames;
    std::vector<std::string> notes;   // as Movie::notes
    bool has_seed = false;
    uint32_t seed = 0;
    bool controller = false;
    std::string thumb;                // the game's picture when it was saved: a small PNG, base64 (optional "thumb"; "" = none)
    // Not saved: the savestate slot holding the game at `frame` while this game runs (see LoadBranch in main.cpp).
    int slot = -1;
    uint32_t slotGen = 0;
};

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
    uint32_t rerecords = 0;           // times history was rewritten: an edit or recording at a row the game had already passed (optional "rerecord_count")
    bool controller = false;          // the game sees one virtual controller (the PAD_* keys); off = it sees none
    std::vector<std::string> notes;   // UTF-8 note per frame ("" = none); frames.size() long once loaded
    Frames prelude;     // embedded prelude (optional)
    std::vector<Branch> branches;     // saved versions (optional "branches" in the file)

    bool Load(const std::wstring& path, std::string& err);
    bool Save(const std::wstring& path, std::string& err) const;
};
