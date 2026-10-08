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
    uint16_t keys[MAX_FRAMES];
};
