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
    bool savestates = false;    // give the game a private heap so SaveState/LoadState work
    bool drop_callbacks = false;    // diagnosis: swallow every XAudio2 callback (needs savestates: the callbacks go through the hook's shims)
    uint32_t quant_hz = 60;          // snap the game's clocks to 1/quant_hz s (60 = one frame): makes timer-driven events land on the same frame at any speed
    bool quant_lock = false;        // experimental: time advances only at frame markers (the game terminates itself a few seconds in)
    uint32_t speed_milli = 1000;    // clock speed while replaying (1000 = 1x)
    uint32_t speed_mask = 31;       // SPEED_* clocks that run at that speed
    bool seeded = false;            // the game sees a frozen clock at Unix time `seed` (its RNG seed)
    uint32_t seed = 0;
    bool hist = false;              // sample the player's values at every frame (the Memory window's history)
    bool rng_log = false;           // log the game's random draws (see the RNG log window)
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
    uint32_t rec_start = 0;         // keys[] index where the current live recording began

    bool Active() const { return s != nullptr; }
    bool Alive() const;
    uint32_t Row() const;           // movie row the held game runs next, or UINT_MAX if running
    // Runs one frame with `keys` as its input and holds again. False if it timed out.
    bool Step(uint16_t keys);
    // Runs n frames in a row without blocking: keys[i] is the input of frame i. The game
    // runs at speed_milli (1000 = real time) and freezes again after the last one.
    bool BeginSteps(const uint16_t* keys, uint32_t n, uint32_t speed_milli);
    // Savestates (need RunParams::savestates, the game frozen). Slots 0..7.
    bool SaveState(int slot);
    bool LoadState(int slot);       // leaves the game frozen at the marker the slot was saved at
    int  StateRow(int slot) const;  // movie row the slot restores to, or -1 if empty
    int PollSteps();                // 1 = done (frozen again), 0 = running, -1 = failed
    uint32_t StepsDone() const;     // frames finished so far by the current batch
    void AbortSteps();              // stops the batch at the next frame boundary
    uint32_t step_target = 0, step_start = 0, step_seen = 0, step_tick = 0;
    // Unfreezes the game and records the live keyboard (the game window must be
    // focused). Frame numbers are movie rows starting at Row() at the time of the call.
    bool StartRecording();
    uint32_t RecCount() const;      // frames recorded so far
    // Freezes the game again at the next frame boundary and returns the recorded
    // frames; stepping can continue afterwards. False if the game did not freeze.
    bool StopRecording(Frames& out);
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
