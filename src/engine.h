// Game process control: launch, hook injection, play/record, save baselines.
#pragma once
#include <windows.h>
#include <stdint.h>
#include <string>
#include <vector>
#include "common.h"
#include "movie.h"

struct RunParams {
    std::wstring exe;           // COTM.exe
    std::wstring dll;           // bscotm_hook.dll
    std::wstring baseline;      // baseline dir copied over the saves first (optional)
    std::wstring backup_dir;    // rolling backup of the user's saves (optional)
    Frames prelude;             // played before the movie (optional)
    Frames movie;               // only the first `target` frames are used
    uint32_t target = 0;        // movie frames to play
    bool record = false;        // after `target` frames, keep recording until stopped
    bool hold = false;          // after `target` frames, freeze the game for frame advance
    uint32_t speed_milli = 1000;    // clock speed while replaying (1000 = 1x)
    uint32_t speed_mask = 31;       // SPEED_* clocks that run at that speed
};

struct RunResult {
    bool ok = false;
    std::string error;
    uint32_t played = 0;        // movie frames played (play mode)
    uint32_t first = 0;         // record mode: movie index of recorded[0]
    Frames recorded;
};

enum Phase { PH_LAUNCH, PH_BOOT, PH_PLAY, PH_RECORD };

struct RunCallbacks {
    void (*progress)(void* ctx, Phase phase, uint32_t movie_frame) = nullptr;
    void* ctx = nullptr;
    volatile LONG* stop = nullptr;      // set non-zero to stop
};

// A live game the editor can step frame by frame (see RunParams::hold).
struct Session {
    HANDLE map = nullptr, proc = nullptr, thread = nullptr;
    Shm* s = nullptr;
    uint32_t pre = 0;               // prelude length, so movie row = marker - 1 - pre

    bool Active() const { return s != nullptr; }
    bool Alive() const;
    uint32_t Row() const;           // movie row the held game runs next, or UINT_MAX if running
    // Runs one frame with `keys` as its input and holds again. False if it timed out.
    bool Step(uint16_t keys);
    // Lets the game run free (live keyboard). The session is closed afterwards.
    void Release();
    void Close();                   // drops our handles; the game keeps running
};

// With p.hold and a non-null `keep`, a successful run leaves the game frozen at
// the frame after `target` and hands the live session over in *keep.
RunResult RunJob(const RunParams& p, const RunCallbacks& cb, Session* keep = nullptr);

bool GameRunning();
void KillGame();

// Copies the game's save files into <baselines>\<id>. The game must be closed.
bool SaveBaseline(const std::wstring& exe, const std::wstring& dir, std::string& err);
