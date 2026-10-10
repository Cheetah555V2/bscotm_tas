// Shared between the host exe and the injected hook DLL.
#pragma once
#include <stdint.h>

// A frame's input: bit i <-> KEYS[i]. Bits 0-15 are keyboard keys (the set the Python tool tracked), bits 16-39 the
// buttons and directions of the virtual controller (see "controller" in hook.cpp), as the game reads them.
typedef uint64_t KeyMask;
enum { NUM_KB_KEYS = 16, NUM_PAD_KEYS = 24, NUM_KEYS = NUM_KB_KEYS + NUM_PAD_KEYS, MAX_FRAMES = 1 << 20 };

struct KeyDef { const char* name; const char* label; uint8_t vk; };    // vk 0 = a controller key
static inline bool IsPadKey(int i) { return i >= NUM_KB_KEYS; }
static inline KeyMask KeyBit(int i) { return (KeyMask)1 << i; }
static const KeyMask PAD_KEYS_MASK = (((KeyMask)1 << NUM_PAD_KEYS) - 1) << NUM_KB_KEYS;

static const KeyDef KEYS[NUM_KEYS] = {
    {"LEFT",  "L",   0x25}, {"RIGHT", "R",   0x27},
    {"UP",    "U",   0x26}, {"DOWN",  "Dn",  0x28},
    {"A",     "A",   0x41}, {"D",     "D",   0x44},
    {"W",     "W",   0x57}, {"S",     "S",   0x53},
    {"SPACE", "Jmp", 0x20}, {"LMB",   "Atk", 0x01},
    {"RMB",   "Sub", 0x02}, {"Q",     "Q",   0x51},
    {"E",     "E",   0x45}, {"P",     "P",   0x50},
    {"ENTER", "Ent", 0x0D}, {"ESC",   "Esc", 0x1B},
    // controller (names are the movie file's keys; bit 16 + n = PAD_* below)
    {"PAD_A", "pA", 0}, {"PAD_B", "pB", 0}, {"PAD_X", "pX", 0}, {"PAD_Y", "pY", 0},
    {"PAD_LB", "LB", 0}, {"PAD_RB", "RB", 0}, {"PAD_BACK", "Bk", 0}, {"PAD_START", "St", 0},
    {"PAD_LS", "LS", 0}, {"PAD_RS", "RS", 0},
    {"PAD_DUP", "dU", 0}, {"PAD_DRIGHT", "dR", 0}, {"PAD_DDOWN", "dD", 0}, {"PAD_DLEFT", "dL", 0},
    {"PAD_LLEFT", "LsL", 0}, {"PAD_LRIGHT", "LsR", 0}, {"PAD_LUP", "LsU", 0}, {"PAD_LDOWN", "LsD", 0},
    {"PAD_RLEFT", "RsL", 0}, {"PAD_RRIGHT", "RsR", 0}, {"PAD_RUP", "RsU", 0}, {"PAD_RDOWN", "RsD", 0},
    {"PAD_LT", "LT", 0}, {"PAD_RT", "RT", 0},
};
// Controller keys, counted from bit 16: buttons 0-9 of the pad (Xbox 360 order), the D-pad (the pad's hat), the left
// and right sticks pushed past the game's threshold (half way), and the triggers (the shared Z axis, pushed nearly fully).
enum PadKey {
    PAD_A, PAD_B, PAD_X, PAD_Y, PAD_LB, PAD_RB, PAD_BACK, PAD_START, PAD_LS, PAD_RS,
    PAD_DUP, PAD_DRIGHT, PAD_DDOWN, PAD_DLEFT, PAD_LLEFT, PAD_LRIGHT, PAD_LUP, PAD_LDOWN,
    PAD_RLEFT, PAD_RRIGHT, PAD_RUP, PAD_RDOWN, PAD_LT, PAD_RT,
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
    volatile uint32_t quant_lock;       // host -> hook: with quant_hz: 3 = frame clock (time is a function of the frame number, see hook.cpp); 2 = clamp the clock to at most 2 steps ahead of the last frame marker; 1 = lock to the markers (experimental: the game exits with code 143)
    volatile uint32_t px_cnt[32];       // diagnosis: calls the game made on its XAudio2 source voices, by vtable slot
    volatile uint32_t sh_cnt[8];        // diagnosis: callbacks XAudio2 delivered, by IXAudio2VoiceCallback method
    volatile uint32_t lockstep;         // host -> hook, experiment: release the game's Sleep-loop thread N times per frame marker (0 = off)
    volatile uint32_t gap_hist[8];      // diagnosis: frame markers by the number of clock-grid steps since the previous one (0, 1, 2, 3, 4+)
    volatile uint32_t gap_n;            // diagnosis: markers (from the 3000th on) whose gap was not exactly 1 step, ...
    volatile uint32_t gap_log[64];      // ... as (marker << 4 | steps), first 64
    uint16_t thr_log[4][12288];         // diagnosis: at each marker (index = marker): cumulative Sleep / Wait calls of the other threads, then of the marker thread
    volatile uint32_t fc_diag[8];       // frame clock (quant_lock 3), hook -> host: [0] frames during which time had to run on by itself (no frame for 100 ms of real time),
                                        // [1] music-thread wakes released, [2] wakes that timed out instead, [3] ticks per frame, [4] QPC frequency, [5] markers that waited for real time
    volatile uint32_t fc_fb_n;          // frame clock diagnosis: reads that got time running on by itself (as above), ...
    volatile uint32_t fc_fb_log[32][3]; // ... the first 32: marker, caller (exe RVA), 1 if the marker thread
    volatile uint32_t pad_mode;         // host -> hook, set before the game starts: 0 = DirectInput untouched, 1 = the game sees no
                                        // controller, 2 = it sees one virtual Xbox 360 pad fed from keys[] (play) or the real pad (record, live)
    volatile uint32_t pad_diag[4];      // hook -> host: [0] real pads found, [1] virtual pad reads, [2] reads that used the real pad,
                                        // [3] the game asked for the controllers (EnumDevices calls)
    KeyMask keys[MAX_FRAMES];
};
