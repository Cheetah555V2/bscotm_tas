// Shared between the host exe and the injected hook DLL.
#pragma once
#include <stdint.h>

enum { NUM_KEYS = 16, MAX_FRAMES = 1 << 20 };

struct KeyDef { const char* name; const char* label; uint8_t vk; };

// Bit i of a frame mask <-> KEYS[i]. Same set the Python tool tracked.
static const KeyDef KEYS[NUM_KEYS] = {
    {"LEFT",  "L",   0x25}, {"RIGHT", "R",   0x27},
    {"UP",    "U",   0x26}, {"DOWN",  "Dn",  0x28},
    {"A",     "A",   0x41}, {"D",     "D",   0x44},
    {"W",     "W",   0x57}, {"S",     "S",   0x53},
    {"SPACE", "Jmp", 0x20}, {"LMB",   "Atk", 0x01},
    {"RMB",   "Sub", 0x02}, {"Q",     "Q",   0x51},
    {"E",     "E",   0x45}, {"P",     "P",   0x50},
    {"ENTER", "Ent", 0x0D}, {"ESC",   "Esc", 0x1B},
};

// Return address of the game's own GetAsyncKeyState poll (COTM.exe RVA).
// Calls from anywhere else (Steam overlay, DirectX) pass through untouched.
static const uint32_t POLL_RET_RVA = 0x2A5960;

enum Mode   : uint32_t { M_IDLE = 0, M_RECORD = 1, M_PLAY = 2 };
enum Status : uint32_t { ST_HOOKED = 1, ST_PLAY_END = 2, ST_RESUMED = 4, ST_HOOK_FAIL = 8, ST_NORENDER_BAD = 16 };

enum Feature : uint32_t { FEAT_SAVESTATE = 1, FEAT_ARENA_OK = 2, FEAT_ARENA_FAIL = 4, FEAT_DROPCB = 8 };

// Clocks the hook can speed up (it patches COTM.exe's own imports of these).
enum SpeedMask : uint32_t {
    SPEED_QPC = 1,      // kernel32!QueryPerformanceCounter
    SPEED_MMTIME = 2,   // winmm!timeGetTime
    SPEED_FILETIME = 4, // kernel32!GetSystemTimeAsFileTime
    SPEED_SLEEP = 8,    // Sleep / WaitForSingleObject(Ex) with a finite timeout
    SPEED_NOVSYNC = 16, // d3d9: force PresentationInterval IMMEDIATE when the device is created
    SPEED_NODRAW = 32,  // d3d9: skip Present/Clear/Draw* while fast-forwarding (see Shm::draw_from)
    SPEED_NORENDER = 64, // skip the game's own render-command queue while fast-forwarding (see SkipDraw)
    SPEED_ALL = 127,
};

#define SHM_NAME "Local\\bscotm_tas_shm"
#define SHM_MAGIC 0x54534342u

// One write to the game's RNG state (a random draw, or the seeding). kind: 1 = the draw function
// COTM.exe+0x80280 (range, result = w % range), 2 = the float version +0x802E0, 0 = any other code
// that writes the state (inlined copies, seeding): `eip` is then the instruction after the write.
enum { RNG_LOG_MAX = 16384 };
struct RngLogEntry { uint32_t frame, eip, ret, range, w, kind; };   // frame = marker number; eip/ret = exe RVAs

// One sample of the player's values, taken by the hook at every frame marker while Shm::hist_on is set.
// frame = marker number; the values are the state before that frame is run, i.e. after frame (marker - 1).
// v: health, weapon points, score (unsigned ints), X speed, Y speed, X, Y, invisibility (float bits);
// 0xFFFFFFFF / NaN means the game has no such value yet (menus, loading).
enum { HIST_MAX = 32768, HIST_FIELDS = 8 };
struct HistEntry { uint32_t frame; uint32_t v[HIST_FIELDS]; };

// Host <-> hook control block. keys[] is the play schedule (host writes) and
// the record buffer (hook writes); index = 0-based frame.
struct Shm {
    uint32_t magic;
    volatile uint32_t mode;         // Mode
    volatile uint32_t frame;        // frame markers seen since the host reset it
    volatile uint32_t stop_at;      // play: number of frames to inject
    volatile uint32_t then_record;  // after stop_at frames, switch to recording
    volatile uint32_t rec_count;    // next keys[] index the recorder writes
    volatile uint32_t polls;        // total game polls (boot detection)
    volatile uint32_t status;       // Status bits
    volatile uint32_t armed;        // record: first marker seen
    volatile uint32_t speed_milli;  // game clock speed, 1000 = 1x (dropped to 1x when play ends)
    volatile uint32_t speed_mask;   // which clocks run at that speed, see SPEED_*
    volatile uint32_t hold;         // block the game at the next frame marker (frame advance)
    volatile uint32_t hold_at;      // arm `hold` when this marker number is reached (0 = off)
    volatile uint32_t advance;      // frames the host releases while held
    volatile uint32_t paused;       // marker number the game is blocked at, 0 = running
    volatile uint32_t draw_from;    // SPEED_NODRAW: skip drawing until 2 frames before this marker (0 = never skip)
    volatile uint32_t cmd_skip_lo;  // SPEED_NORENDER: bit i = skip render-command type i (0..31); see hook.cpp
    volatile uint32_t cmd_skip_hi;  // types 32..35 in bits 0..3
    volatile uint32_t cmd_tail;     // SPEED_NORENDER: stop skipping this many frames before draw_from
    volatile uint32_t cmd_count[36]; // SPEED_NORENDER: how many commands of each type were skipped (statistics)
    volatile uint32_t rng_on;       // 1 = the game sees a frozen clock at Unix time rng_time (that is its RNG seed)
    volatile uint32_t rng_time;     // Unix time in seconds; the game seeds its xorshift128 generator from it at launch
    volatile uint32_t rng_log;      // 1 = log every write to the game's RNG state (see hook.cpp, RngLogEntry)
    volatile uint32_t rng_log_n;    // entries written so far; entry i lives at rng_log_buf[i % RNG_LOG_MAX]
    RngLogEntry rng_log_buf[RNG_LOG_MAX];
    volatile uint32_t hist_on;      // 1 = sample the player's values at every frame marker (see HistEntry)
    volatile uint32_t hist_n;       // samples taken so far; sample i lives at hist_buf[i % HIST_MAX]
    HistEntry hist_buf[HIST_MAX];
    volatile uint32_t features;     // FEAT_* bits: host sets FEAT_SAVESTATE before injection, hook adds FEAT_ARENA_OK/FAIL
    volatile uint32_t snap_cmd;     // host -> hook (while held): 1 = save slot, 2 = load slot; hook clears it when done
    volatile uint32_t snap_slot;
    volatile uint32_t snap_result;  // 1 = ok, 2 = failed
    volatile uint32_t snap_frame[8];// marker number each slot was saved at (0 = empty)
    volatile uint32_t snap_diag[8]; // hook -> host: [0] threads captured at the last save, [1] restored at the last load, [2] mismatched (skipped), [3] missing, [4]/[5] first mismatch saved/now eip
    volatile uint32_t snap_stats[4];    // hook -> host, per save: [0] pages in the pool, [1] pages that had to be copied, [2] pages in the state, [3] pool pages free
    volatile uint32_t snap_time[8];     // hook -> host, ms per phase of the last save: [0] engine pause, [1] park threads, [2] compare pages, [3] reserve pool, [4] open+freeze, [5] copy, [6] total in Save, [7] engine resume
    volatile uint32_t quant_hz;         // host -> hook: snap the clocks the game reads to a grid of 1/quant_hz s (0 = off)
    volatile uint32_t quant_lock;       // host -> hook: with quant_hz, also lock time to the frame markers (experimental: the game exits with code 143)
    volatile uint32_t px_cnt[32];       // diagnosis: calls the game made on its XAudio2 source voices, by vtable slot
    volatile uint32_t sh_cnt[8];        // diagnosis: callbacks XAudio2 delivered, by IXAudio2VoiceCallback method
    uint16_t keys[MAX_FRAMES];
};
