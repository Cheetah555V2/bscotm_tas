// bscotm_hook.dll - injected into COTM.exe.
//
// Patches COTM.exe's own import slots (the game never lets us touch its .text):
//  - user32!GetAsyncKeyState: only calls whose return address is the game's
//    input poll are recorded / overridden.
//  - the clocks the game can read (QPC, timeGetTime, GetSystemTimeAsFileTime,
//    Sleep, WaitForSingleObject): scaled by Shm::speed_milli so playback can
//    fast-forward. Calls made by other modules (D3D, XAudio, Steam) are untouched.
#include <windows.h>
#include <string.h>
#include <utility>
#include <tlhelp32.h>
#include "common.h"
#include "savestate.h"
static Shm*      S;
static uintptr_t PollRet;
static uint8_t   BitOf[256];
static uint32_t  Cur;               // keys held during the current frame

static SHORT (WINAPI *R_Gaks)(int);
static BOOL  (WINAPI *R_Qpc)(LARGE_INTEGER*);
static DWORD (WINAPI *R_Tgt)(void);
static void  (WINAPI *R_Ft)(FILETIME*);
static void  (WINAPI *R_Sleep)(DWORD);
static DWORD (WINAPI *R_Wait)(HANDLE, DWORD);
static DWORD (WINAPI *R_WaitEx)(HANDLE, DWORD, BOOL);

// ---- virtual clock ---------------------------------------------------------
// virtual = VirtBase + (real - RealBase) * speed. Re-based whenever the speed
// changes, so time never jumps and never runs backwards.
static CRITICAL_SECTION Cs;
static int64_t Freq, RealBase, VirtBase, V0;
static uint32_t CurSpeed = 1000, T0ms;
static uint64_t Ft0;

static uint32_t Speed() { uint32_t s = S->speed_milli; return s ? s : 1000; }

static int64_t VNow(int64_t real) {
    uint32_t sp = Speed();
    EnterCriticalSection(&Cs);
    if (sp != CurSpeed) {
        VirtBase += (real - RealBase) * CurSpeed / 1000;
        RealBase = real;
        CurSpeed = sp;
    }
    int64_t v = VirtBase + (real - RealBase) * CurSpeed / 1000;
    LeaveCriticalSection(&Cs);
    return v;
}

// With Shm::quant_hz set, the clocks the game reads are made independent of thread scheduling. Until the first frame
// marker they are snapped down to a grid of 1/quant_hz s; from then on time is frame-locked: it advances only at frame
// markers, by exactly one grid step each, so the time the game measures between two frames is the same on every run,
// at every speed, with or without pauses. (A pure function of the frame number, so a savestate load gives the same clock.)
static volatile LONG LockOn;
static int64_t LockBase, LockServed, LockMarkReal;
static uint32_t LockF0;
static int64_t Quant(int64_t v) {
    uint32_t hz = S->quant_hz;
    if (!hz) return v;
    int64_t step = Freq / hz;
    if (S->quant_lock >= 2) {           // clamp: the clock may run at most quant_lock grid steps ahead of what the last frame marker was served (stalls cannot lengthen a measured frame)
        if (v < V0) return v;
        int64_t g = V0 + (v - V0) / step * step;
        if (!LockOn) return g;
        EnterCriticalSection(&Cs);
        int64_t cap = LockServed + (int64_t)S->quant_lock * step, r = g;
        LARGE_INTEGER rn; R_Qpc(&rn);
        if (g > cap && rn.QuadPart - LockMarkReal < Freq * 4) r = cap;      // no marker for 4 s of real time (a hung or very long load): let the clock run; any release costs determinism
        if (r < LockServed) r = LockServed;
        LeaveCriticalSection(&Cs);
        return r;
    }
    if (LockOn) {
        int64_t t = LockBase + (int64_t)(int32_t)(S->frame - LockF0) * step;
        if (!S->paused) {           // no marker for a while (loading, a menu transition): let time run on the grid so nothing stalls
            LARGE_INTEGER n;
            R_Qpc(&n);
            int64_t since = n.QuadPart - LockMarkReal, grace = Freq / 33;
            if (since > grace) t += (since - grace) * Speed() / 1000 / step * step;
        }
        if (t < LockServed) t = LockServed; else LockServed = t;
        return t;
    }
    if (v < V0) return v;
    return V0 + (v - V0) / step * step;
}
// At every frame marker: start the frame-locked clock at the first one; later keep it from running behind what the game was told.
static void LockAtMarker(uint32_t f, int64_t vnow) {
    if (!S->quant_hz || !S->quant_lock) return;
    int64_t step = Freq / S->quant_hz;
    if (S->quant_lock >= 2) {
        int64_t t = Quant(vnow);
        if (!LockOn) { LockServed = t; MemoryBarrier(); LockOn = 1; return; }
        EnterCriticalSection(&Cs);
        if (t > LockServed) LockServed = t;
        { LARGE_INTEGER mn; R_Qpc(&mn); LockMarkReal = mn.QuadPart; }
        LeaveCriticalSection(&Cs);
        return;
    }
    LARGE_INTEGER n;
    R_Qpc(&n);
    if (!LockOn) {
        LockBase = V0 + (vnow - V0) / step * step;
        LockF0 = f;
        LockServed = LockBase;
        LockMarkReal = n.QuadPart;
        MemoryBarrier();
        LockOn = 1;
        return;
    }
    int64_t t = LockBase + (int64_t)(int32_t)(f - LockF0) * step;
    if (LockServed > t) LockBase += (LockServed - t + step - 1) / step * step;     // time never runs backwards
    LockMarkReal = n.QuadPart;
}
static BOOL WINAPI H_Qpc(LARGE_INTEGER* o) {
    BOOL r = R_Qpc(o);
    if (r && (S->speed_mask & SPEED_QPC)) o->QuadPart = Quant(VNow(o->QuadPart));
    return r;
}

static DWORD WINAPI H_Tgt() {
    DWORD r = R_Tgt();
    if (!(S->speed_mask & SPEED_MMTIME)) return r;
    LARGE_INTEGER q;
    R_Qpc(&q);
    return T0ms + (DWORD)((Quant(VNow(q.QuadPart)) - V0) * 1000 / Freq);
}

// RNG seed: the game seeds its generator from the Unix time at launch. Its static C runtime reads the
// clock through GetProcAddress (not through its imports), so hand out our clock from there too.
static void SeedTime(FILETIME* f) {
    uint64_t t = (uint64_t)S->rng_time * 10000000ull + 116444736000000000ull;
    f->dwLowDateTime = (DWORD)t; f->dwHighDateTime = (DWORD)(t >> 32);
}
static FARPROC (WINAPI *R_Gpa)(HMODULE, LPCSTR);
static void WINAPI H_FtSeed(FILETIME* f) { SeedTime(f); }
static FARPROC WINAPI H_Gpa(HMODULE m, LPCSTR name) {
    FARPROC p = R_Gpa(m, name);
    if (p && S->rng_on && ((uintptr_t)name >> 16) &&
        (!_stricmp(name, "GetSystemTimePreciseAsFileTime") || !_stricmp(name, "GetSystemTimeAsFileTime")))
        return (FARPROC)H_FtSeed;
    return p;
}

static void WINAPI H_Ft(FILETIME* f) {
    if (S->rng_on) { SeedTime(f); return; }
    R_Ft(f);
    if (!(S->speed_mask & SPEED_FILETIME)) return;
    LARGE_INTEGER q;
    R_Qpc(&q);
    uint64_t t = Ft0 + (uint64_t)((Quant(VNow(q.QuadPart)) - V0) * 10000000 / Freq);
    f->dwLowDateTime = (DWORD)t;
    f->dwHighDateTime = (DWORD)(t >> 32);
}

static DWORD Shorten(DWORD ms) {
    if (!(S->speed_mask & SPEED_SLEEP) || !ms || ms == INFINITE) return ms;
    uint32_t sp = Speed();
    if (sp <= 1000) return ms;
    DWORD n = (DWORD)((uint64_t)ms * 1000 / sp);
    return n ? n : 1;
}

// Exact scaled waits. At speed s a game wait of `ms` should take ms/s of real time. Rounding that up to whole
// milliseconds (Sleep(1)) would be 50 ms of game time at 50x, i.e. several frames, and the game would measure
// those frames as long: timers that add up the measured frame time would run fast. So the time left under ~4 ms of real time is
// waited for by polling instead of sleeping.
static bool PreciseTarget(DWORD ms, int64_t& target) {
    if (!(S->speed_mask & SPEED_SLEEP) || !ms || ms == INFINITE) return false;
    uint32_t sp = Speed();
    if (sp <= 1000) return false;
    LARGE_INTEGER n;
    R_Qpc(&n);
    target = n.QuadPart + (int64_t)ms * Freq / sp;
    return true;
}
template <class F> static DWORD PreciseWait(DWORD ms, F wait) {
    int64_t tgt;
    if (!PreciseTarget(ms, tgt)) return wait(ms);
    for (;;) {
        LARGE_INTEGER n;
        R_Qpc(&n);
        int64_t rem = tgt - n.QuadPart;
        if (rem <= 0) return wait(0);                  // a last look: WAIT_TIMEOUT if still not signalled
        DWORD slice = rem > Freq / 250 ? (DWORD)(rem * 1000 / Freq) - 1 : 0;      // more than 4 ms left: block for most of it
        DWORD r = wait(slice);
        if (r != WAIT_TIMEOUT) return r;
        if (!slice) SwitchToThread();
    }
}

// Threads that poll in a Sleep loop (the game has one that does Sleep(8) for its whole life) are counted
// here so JoinThreads does not wait for them to "finish": they never do, and waiting cost 3 s per launch.
static const int MAXSLEEPERS = 64;
static struct { volatile DWORD tid; volatile LONG n; } Sleepers[MAXSLEEPERS];
static void NoteSleep() {
    DWORD me = GetCurrentThreadId();
    for (int i = 0; i < MAXSLEEPERS; i++) {
        if (Sleepers[i].tid == me) { InterlockedIncrement(&Sleepers[i].n); return; }
        if (!Sleepers[i].tid) {
            if (InterlockedCompareExchange((volatile LONG*)&Sleepers[i].tid, (LONG)me, 0) == 0 || Sleepers[i].tid == me) { InterlockedIncrement(&Sleepers[i].n); return; }
        }
    }
}
static bool IsSleeper(DWORD tid) {
    for (int i = 0; i < MAXSLEEPERS && Sleepers[i].tid; i++) if (Sleepers[i].tid == tid) return Sleepers[i].n >= 3;
    return false;
}
// ---- idle gate: lets a savestate stop the game's own threads at a safe point ------------
// A background thread of the game (it loops on Sleep(8)) runs game code and takes the game's
// locks. Copying or rewriting memory under it would leave a lock half taken. So these
// threads are tracked, count as idle while inside a Sleep/Wait, and when the gate is closed
// they park on the way out of the call. A savestate proceeds only once all of them are idle.
struct GThread { DWORD tid; HANDLE h; volatile LONG idle, quiet; };    // idle: inside a Sleep/Wait; quiet: inside a plain Sleep, i.e. between jobs
static GThread GT[128];
static volatile LONG NGT;
static volatile LONG GateClosed;
static DWORD GateTls = TLS_OUT_OF_INDEXES;

static GThread* GateMe() {
    if (GateTls == TLS_OUT_OF_INDEXES) return nullptr;
    GThread* g = (GThread*)TlsGetValue(GateTls);
    if (g) return g;
    DWORD me = GetCurrentThreadId();
    for (LONG i = 0; i < NGT; i++) if (GT[i].tid == me) { TlsSetValue(GateTls, &GT[i]); return &GT[i]; }
    return nullptr;
}
static inline void GateEnter(GThread* g, bool quiet = false) { if (g) { g->quiet = quiet; g->idle = 1; MemoryBarrier(); } }
static void GateLeave(GThread* g, bool quiet = false) {          // about to run game code again; quiet: it came out of a plain Sleep
    if (!g) return;
    for (;;) {
        g->idle = 0; g->quiet = 0;
        MemoryBarrier();
        if (!GateClosed) return;
        g->quiet = quiet;                     // parked in the gate: as quiet as the call it came out of (a thread that woke from Sleep(8) is between jobs)
        g->idle = 1;
        MemoryBarrier();
        while (GateClosed) Sleep(1);
    }
}
static void GateRegister(HANDLE game_handle, DWORD tid) {
    HANDLE d;
    if (NGT >= 128 || !DuplicateHandle(GetCurrentProcess(), game_handle, GetCurrentProcess(), &d, SYNCHRONIZE, FALSE, 0)) return;
    GT[NGT].tid = tid; GT[NGT].h = d; GT[NGT].idle = 0;
    MemoryBarrier();
    NGT++;
}
static bool ParkAll() {
    GateClosed = 1;
    MemoryBarrier();
    for (DWORD t0 = GetTickCount(); GetTickCount() - t0 < 2000;) {
        bool all = true;
        for (LONG i = 0; i < NGT; i++)
            if (!(GT[i].idle && GT[i].quiet) && WaitForSingleObject(GT[i].h, 0) != WAIT_OBJECT_0) all = false;     // a thread in a wait is not enough: it may be half way through a job
        if (all) return true;
        Sleep(0);
    }
    return false;
}
static void UnparkAll() { GateClosed = 0; MemoryBarrier(); }
static DWORD GameTidList(DWORD* out, DWORD max) {
    DWORD n = 0;
    for (LONG i = 0; i < NGT && n < max; i++) out[n++] = GT[i].tid;
    return n;
}

static volatile LONG MarkerTid;                        // diagnosis: the thread that runs the frame markers, and how often each side sleeps / waits
static volatile LONG ThrCnt[4];                      // other-thread Sleep, other-thread Wait, marker-thread Sleep, marker-thread Wait
static inline void NoteThr(int wait) { ThrCnt[((LONG)GetCurrentThreadId() == MarkerTid ? 2 : 0) + wait]++; }
struct SleepSite { uint32_t ret, ms, n; };
static SleepSite SleepSites[16];                       // diagnosis: the tracked game thread's Sleep calls by call site and duration
static void NoteSleepSite(uint32_t ret, uint32_t ms) {
    for (int i = 0; i < 16; i++) {
        if (SleepSites[i].ret == ret && SleepSites[i].ms == ms) { SleepSites[i].n++; return; }
        if (!SleepSites[i].ret) { SleepSites[i] = { ret, ms, 1 }; return; }
    }
}
// Experiment (Shm::lockstep = N): a background thread that polls in a short Sleep loop is released by the frame marker, N times
// per frame and one wake at a time, so its work happens at fixed points of the frame instead of whenever the scheduler runs it.
static HANDLE LsSem;
static volatile LONG LsEpoch, LsParked;
static void LockstepWait() {
    InterlockedIncrement(&LsEpoch);
    LsParked = 1;
    MemoryBarrier();
    R_Wait(LsSem, 100);
    LsParked = 0;
}
static void LockstepRelease(uint32_t n) {
    for (uint32_t k = 0; k < n && LsParked; k++) {
        LONG e0 = LsEpoch;
        ReleaseSemaphore(LsSem, 1, nullptr);
        LARGE_INTEGER a, b; R_Qpc(&a);
        while (LsEpoch == e0) { R_Qpc(&b); if (b.QuadPart - a.QuadPart > Freq * 5) return; SwitchToThread(); }
    }
}
static void  WINAPI H_Sleep(DWORD ms) {
    if (ms) NoteSleep();
    NoteThr(0);
    GThread* g = GateMe();
    if (g) NoteSleepSite((uint32_t)(uintptr_t)__builtin_return_address(0), ms);
    GateEnter(g, true);
    if (S->lockstep && LsSem && ms && ms <= 16 && (LONG)GetCurrentThreadId() != MarkerTid && IsSleeper(GetCurrentThreadId())) LockstepWait();
    else PreciseWait(ms, [&](DWORD t) { R_Sleep(t); return (DWORD)WAIT_TIMEOUT; });
    GateLeave(g, true);
}
static DWORD WINAPI H_Wait(HANDLE h, DWORD ms) {
    GThread* g = GateMe();
    GateEnter(g);
    NoteThr(1);
    DWORD r = PreciseWait(ms, [&](DWORD t) { return R_Wait(h, t); });
    GateLeave(g);
    return r;
}
static DWORD WINAPI H_WaitEx(HANDLE h, DWORD ms, BOOL a) {
    GThread* g = GateMe();
    GateEnter(g);
    NoteThr(1);
    DWORD r = PreciseWait(ms, [&](DWORD t) { return R_WaitEx(h, t, a); });
    GateLeave(g);
    return r;
}

// ---- d3d9: optionally drop vsync -----------------------------------------------
// Direct3DCreate9 is wrapped so we can patch IDirect3D9::CreateDevice (slot 16 of
// d3d9.dll's own vtable, not game code) and rewrite the present interval.
static void* (WINAPI *R_D3dCreate)(UINT);
static HRESULT (WINAPI *R_CreateDevice)(void*, UINT, UINT, HWND, DWORD, DWORD*, void**);

// With SPEED_NODRAW the frames nobody will see are not drawn at all: Present, Clear and the
// Draw* calls (all in d3d9.dll, not game code) return success without doing anything, which
// is most of the per-frame cost. The last 2 frames before the host's stopping point are drawn
// normally so the frozen picture is correct.
static bool SkipDraw() {
    return (S->speed_mask & SPEED_NODRAW) && S->draw_from && Speed() > 1000 && S->frame + 2 < S->draw_from;
}

// With SPEED_NORENDER the game's own render-command queue is not executed either. The game
// queues drawing commands during a frame and runs them from a loop that calls handler
// functions through a table in its data section (COTM.exe+0x380D88, 36 entries, called as
// `push cmd; call [table + type*4]`, handlers return with `ret 4`). While frames are skipped
// (see SkipDraw) every entry is pointed at a stub that only returns; the originals are put
// back before the frames that will be seen. The table is data, not code, so this is a data write.
static const uint32_t CMD_TABLE_RVA = 0x380D88, CMD_COUNT = 36;
// Types skipped by default: those the game issues at least once a frame whose skipping changes neither
// its state nor the final picture (checked on test3.bscotm). Types 6, 7, 14, 19, 20 and 27 load resources
// or set persistent device state (skipping them leaves a black or wrong picture), 10 is needed to get past
// the boot, and the rarely used (12, 21, 29) or never seen (0, 5, 8) ones are left alone to be safe.
static const uint64_t CMD_SKIP_DEFAULT = 0x0000000FD7C7AA1EULL;
static uint32_t* CmdTab;
static uint32_t  CmdOrig[CMD_COUNT];
static bool      CmdReady, CmdOff;
// One stub per command type (stdcall with one argument pops the same 4 bytes the handlers do),
// so the host can see which types were skipped.
template <int I> static void __stdcall CmdStub(void*) { S->cmd_count[I]++; }
template <int... I> static const void* const* StubTable(std::integer_sequence<int, I...>) {
    static const void* const t[] = {(const void*)&CmdStub<I>...};
    return t;
}
static void SetCmdSkip(bool on) {
    if (on == CmdOff) return;
    if (!CmdReady) {
        BYTE* base = (BYTE*)GetModuleHandleW(NULL);
        auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
        uint32_t lo = (uint32_t)(uintptr_t)base, hi = lo + nt->OptionalHeader.SizeOfImage;
        uint32_t* t = (uint32_t*)(base + CMD_TABLE_RVA);
        for (uint32_t i = 0; i < CMD_COUNT; i++)         // every entry must be a function inside the exe
            if (t[i] < lo || t[i] >= hi) { CmdReady = CmdOff = false; S->status |= ST_NORENDER_BAD; return; }
        memcpy(CmdOrig, t, sizeof CmdOrig);
        CmdTab = t;
        CmdReady = true;
    }
    DWORD old;
    if (!VirtualProtect(CmdTab, sizeof CmdOrig, PAGE_READWRITE, &old)) return;
    static const void* const* stubs = StubTable(std::make_integer_sequence<int, CMD_COUNT>());
    uint64_t m = ((uint64_t)S->cmd_skip_hi << 32) | S->cmd_skip_lo;
    if (!m) m = CMD_SKIP_DEFAULT;
    for (uint32_t i = 0; i < CMD_COUNT; i++) CmdTab[i] = (on && (m >> i & 1)) ? (uint32_t)(uintptr_t)stubs[i] : CmdOrig[i];
    VirtualProtect(CmdTab, sizeof CmdOrig, old, &old);
    CmdOff = on;
}
static void UpdateCmdSkip() {
    uint32_t tail = S->cmd_tail ? S->cmd_tail : 2;
    SetCmdSkip((S->speed_mask & SPEED_NORENDER) && SkipDraw() && S->frame + tail < S->draw_from);
}
typedef HRESULT (__stdcall *PFN_Present)(void*, const void*, const void*, HWND, const void*);
typedef HRESULT (__stdcall *PFN_Clear)(void*, DWORD, const void*, DWORD, DWORD, float, DWORD);
typedef HRESULT (__stdcall *PFN_DP)(void*, UINT, UINT, UINT);
typedef HRESULT (__stdcall *PFN_DIP)(void*, UINT, INT, UINT, UINT, UINT, UINT);
typedef HRESULT (__stdcall *PFN_DPUP)(void*, UINT, UINT, const void*, UINT);
typedef HRESULT (__stdcall *PFN_DIPUP)(void*, UINT, UINT, UINT, UINT, const void*, UINT, const void*, UINT);
static PFN_Present R_Present; static PFN_Clear R_Clear; static PFN_DP R_Dp; static PFN_DIP R_Dip;
static PFN_DPUP R_Dpup; static PFN_DIPUP R_Dipup;
static HRESULT __stdcall H_Present(void* d, const void* a, const void* b, HWND w, const void* c) { return SkipDraw() ? 0 : R_Present(d, a, b, w, c); }
static HRESULT __stdcall H_Clear(void* d, DWORD n, const void* r, DWORD f, DWORD c, float z, DWORD s) { return SkipDraw() ? 0 : R_Clear(d, n, r, f, c, z, s); }
static HRESULT __stdcall H_Dp(void* d, UINT t, UINT s, UINT n) { return SkipDraw() ? 0 : R_Dp(d, t, s, n); }
static HRESULT __stdcall H_Dip(void* d, UINT t, INT b, UINT mi, UINT nv, UINT si, UINT pc) { return SkipDraw() ? 0 : R_Dip(d, t, b, mi, nv, si, pc); }
static HRESULT __stdcall H_Dpup(void* d, UINT t, UINT pc, const void* v, UINT st) { return SkipDraw() ? 0 : R_Dpup(d, t, pc, v, st); }
static HRESULT __stdcall H_Dipup(void* d, UINT t, UINT mi, UINT nv, UINT pc, const void* i, UINT fmt, const void* v, UINT st) {
    return SkipDraw() ? 0 : R_Dipup(d, t, mi, nv, pc, i, fmt, v, st);
}

// ---- immortal D3D9 resources (savestates) --------------------------------------------------
// A restore puts the game's memory back to a moment when it still held pointers to D3D objects that the
// game released afterwards. Releasing such a pointer again (the restored game does, exactly as it did the
// first time) would hit freed memory. So with savestates on, a D3D resource is never destroyed: when the
// game releases what would be the last reference, the object is kept (one reference stays with us) and the
// game is told it is gone. The price is video memory that is not given back; a restore is safe.
struct ZClass { void** vt; ULONG (__stdcall *addref)(void*); ULONG (__stdcall *release)(void*); };
static ZClass ZC[64];
static volatile LONG NZC;
static volatile LONG ZombieKept;                     // releases swallowed (statistics)
static ZClass* ZFind(void* obj) {
    void** vt = *(void***)obj;
    for (LONG i = 0; i < NZC; i++) if (ZC[i].vt == vt) return &ZC[i];
    return nullptr;
}
static ULONG __stdcall H_ZRelease(void* self) {
    ZClass* z = ZFind(self);
    if (!z) return 0;
    ULONG c = z->addref(self);                       // the count including this temporary reference
    if (c <= 2) { z->release(self); InterlockedIncrement(&ZombieKept); return 0; }
    z->release(self);
    return z->release(self);
}
static void ZPatch(void* obj) {
    if (!obj || ZFind(obj) || NZC >= 64) return;
    void** vt = *(void***)obj;
    DWORD old;
    if (!VirtualProtect(&vt[1], 8, PAGE_READWRITE, &old)) return;
    ZC[NZC].vt = vt;
    ZC[NZC].addref = (ULONG (__stdcall*)(void*))vt[1];
    ZC[NZC].release = (ULONG (__stdcall*)(void*))vt[2];
    MemoryBarrier();
    NZC++;
    vt[2] = (void*)H_ZRelease;
    VirtualProtect(&vt[1], 8, old, &old);
}
static void* DevOrig[128];                           // original device methods by vtable slot
#define ZPOST(out) do { void** pp = (void**)(out); if (hr == 0 && pp && *pp) ZPatch(*pp); } while (0)
#define ZH1(name, slot, o) static HRESULT __stdcall name(void* d, uintptr_t a0) { HRESULT hr = ((HRESULT (__stdcall*)(void*, uintptr_t))DevOrig[slot])(d, a0); \
    uintptr_t a[] = {a0}; ZPOST(a[o]); return hr; }
#define ZH2(name, slot, o) static HRESULT __stdcall name(void* d, uintptr_t a0, uintptr_t a1) { HRESULT hr = ((HRESULT (__stdcall*)(void*, uintptr_t, uintptr_t))DevOrig[slot])(d, a0, a1); \
    uintptr_t a[] = {a0, a1}; ZPOST(a[o]); return hr; }
#define ZH6(name, slot, o) static HRESULT __stdcall name(void* d, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5) { \
    HRESULT hr = ((HRESULT (__stdcall*)(void*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t))DevOrig[slot])(d, a0, a1, a2, a3, a4, a5); \
    uintptr_t a[] = {a0, a1, a2, a3, a4, a5}; ZPOST(a[o]); return hr; }
#define ZH7(name, slot, o) static HRESULT __stdcall name(void* d, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6) { \
    HRESULT hr = ((HRESULT (__stdcall*)(void*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t))DevOrig[slot])(d, a0, a1, a2, a3, a4, a5, a6); \
    uintptr_t a[] = {a0, a1, a2, a3, a4, a5, a6}; ZPOST(a[o]); return hr; }
#define ZH8(name, slot, o) static HRESULT __stdcall name(void* d, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7) { \
    HRESULT hr = ((HRESULT (__stdcall*)(void*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t))DevOrig[slot])(d, a0, a1, a2, a3, a4, a5, a6, a7); \
    uintptr_t a[] = {a0, a1, a2, a3, a4, a5, a6, a7}; ZPOST(a[o]); return hr; }
#define ZH9(name, slot, o) static HRESULT __stdcall name(void* d, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8) { \
    HRESULT hr = ((HRESULT (__stdcall*)(void*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t))DevOrig[slot])(d, a0, a1, a2, a3, a4, a5, a6, a7, a8); \
    uintptr_t a[] = {a0, a1, a2, a3, a4, a5, a6, a7, a8}; ZPOST(a[o]); return hr; }
// argument index of the out pointer (IDirect3DXxx9** pp) is the third parameter of each macro
ZH8(Z_CreateTexture, 23, 6)          // Width Height Levels Usage Format Pool ppTexture pSharedHandle
ZH9(Z_CreateVolumeTexture, 24, 7)    // W H Depth Levels Usage Format Pool ppVolumeTexture pSharedHandle
ZH7(Z_CreateCubeTexture, 25, 5)      // Edge Levels Usage Format Pool ppCubeTexture pSharedHandle
ZH6(Z_CreateVertexBuffer, 26, 4)     // Length Usage FVF Pool ppVertexBuffer pSharedHandle
ZH6(Z_CreateIndexBuffer, 27, 4)      // Length Usage Format Pool ppIndexBuffer pSharedHandle
ZH8(Z_CreateRenderTarget, 28, 6)     // W H Format MultiSample MultisampleQuality Lockable ppSurface pSharedHandle
ZH8(Z_CreateDepthStencil, 29, 6)     // W H Format MultiSample MultisampleQuality Discard ppSurface pSharedHandle
ZH6(Z_CreateOffscreen, 36, 4)        // W H Format Pool ppSurface pSharedHandle
ZH2(Z_CreateStateBlock, 59, 1)       // Type ppSB
ZH1(Z_EndStateBlock, 61, 0)          // ppSB
ZH2(Z_CreateVertexDecl, 86, 1)       // pVertexElements ppDecl
ZH2(Z_CreateVertexShader, 91, 1)     // pFunction ppShader
ZH2(Z_CreatePixelShader, 106, 1)     // pFunction ppShader
ZH2(Z_CreateQuery, 118, 1)           // Type ppQuery
static void PatchDeviceCreators(void* dev) {
    void** vt = *(void***)dev;
    struct { int slot; void* hook; } t[] = {
        {23, (void*)Z_CreateTexture}, {24, (void*)Z_CreateVolumeTexture}, {25, (void*)Z_CreateCubeTexture}, {26, (void*)Z_CreateVertexBuffer},
        {27, (void*)Z_CreateIndexBuffer}, {28, (void*)Z_CreateRenderTarget}, {29, (void*)Z_CreateDepthStencil}, {36, (void*)Z_CreateOffscreen},
        {59, (void*)Z_CreateStateBlock}, {61, (void*)Z_EndStateBlock}, {86, (void*)Z_CreateVertexDecl}, {91, (void*)Z_CreateVertexShader},
        {106, (void*)Z_CreatePixelShader}, {118, (void*)Z_CreateQuery},
    };
    for (auto& e : t) {
        if (DevOrig[e.slot]) continue;                  // shared vtable, already done
        DWORD old;
        if (!VirtualProtect(&vt[e.slot], 4, PAGE_READWRITE, &old)) continue;
        DevOrig[e.slot] = vt[e.slot];
        vt[e.slot] = e.hook;
        VirtualProtect(&vt[e.slot], 4, old, &old);
    }
}

static void PatchDevice(void* dev) {
    void** vt = *(void***)dev;
    struct { int slot; void* hook; void* real; } t[] = {
        {17, (void*)H_Present, &R_Present}, {43, (void*)H_Clear, &R_Clear}, {81, (void*)H_Dp, &R_Dp},
        {82, (void*)H_Dip, &R_Dip}, {83, (void*)H_Dpup, &R_Dpup}, {84, (void*)H_Dipup, &R_Dipup},
    };
    for (auto& e : t) {
        if (*(void**)e.real) continue;                  // already patched (shared vtable)
        DWORD old;
        if (!VirtualProtect(&vt[e.slot], 4, PAGE_READWRITE, &old)) continue;
        *(void**)e.real = vt[e.slot];
        vt[e.slot] = e.hook;
        VirtualProtect(&vt[e.slot], 4, old, &old);
    }
}

static HRESULT WINAPI H_CreateDevice(void* self, UINT ad, UINT type, HWND wnd, DWORD fl, DWORD* pp, void** out) {
    // D3DPRESENT_PARAMETERS: PresentationInterval is the 14th DWORD.
    if ((S->speed_mask & SPEED_NOVSYNC) && pp) pp[13] = 0x80000000u;   // D3DPRESENT_INTERVAL_IMMEDIATE
    HRESULT hr = R_CreateDevice(self, ad, type, wnd, fl, pp, out);
    if (hr == 0 && out && *out) { PatchDevice(*out); if (S->features & FEAT_SAVESTATE) PatchDeviceCreators(*out); }
    return hr;
}

static void* WINAPI H_D3dCreate(UINT ver) {
    void* d3d = R_D3dCreate(ver);
    if (d3d && !R_CreateDevice) {
        void** vt = *(void***)d3d;
        DWORD old;
        if (VirtualProtect(&vt[16], 4, PAGE_READWRITE, &old)) {
            R_CreateDevice = (HRESULT (WINAPI*)(void*, UINT, UINT, HWND, DWORD, DWORD*, void**))vt[16];
            vt[16] = (void*)H_CreateDevice;
            VirtualProtect(&vt[16], 4, old, &old);
        }
    }
    return d3d;
}

// ---- focus ---------------------------------------------------------------------
// The game only accepts input while its window is active (it learns that from window
// messages; it imports no focus query), so scripted input stopped working the moment
// the editor had focus. We wrap its window procedure so it never sees a deactivation
// and tell it once that it is active. Live keys are then gated here instead, on the
// window really being the foreground window, so typing in the editor cannot leak in.
static HWND    GameWnd;
static WNDPROC OrigProc;
static bool    Focused = true;
static HWND (WINAPI *R_Cwe)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);

static LRESULT CALLBACK H_Wnd(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_ACTIVATEAPP: case WM_NCACTIVATE: w = TRUE; break;
        case WM_ACTIVATE: w = WA_ACTIVE; break;
        case WM_KILLFOCUS: return 0;
    }
    return CallWindowProcA(OrigProc, h, m, w, l);
}

static HWND WINAPI H_Cwe(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h,
                         HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    HWND wnd = R_Cwe(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    if (wnd && !parent && !GameWnd) {       // the game's top-level window
        GameWnd = wnd;
        OrigProc = (WNDPROC)(uintptr_t)SetWindowLongA(wnd, GWL_WNDPROC, (LONG)(uintptr_t)H_Wnd);
        PostMessageA(wnd, WM_ACTIVATEAPP, TRUE, 0);
        PostMessageA(wnd, WM_ACTIVATE, WA_ACTIVE, 0);
        PostMessageA(wnd, WM_SETFOCUS, 0, 0);
    }
    return wnd;
}

// ---- RNG draw log ---------------------------------------------------------------------------------
// The game's xorshift128 state (x,y,z,w) lives at *(COTM.exe+0x48365C) + 0x2F4..0x300 and every draw
// writes all four words. A hardware write breakpoint (debug register 0, set on every thread of the
// process; no game code is touched) on the last word traps each draw, whether it goes through the draw
// function (+0x80280 / float +0x802E0) or one of the ~100 inlined copies. Each trap is recorded in
// Shm::rng_log_buf. It is only armed while the host wants the log and the frames are near the stopping
// point (every trap costs microseconds), see RngLogWanted.
static uint32_t  RngWatch;                          // address currently watched, 0 = nothing
static const uint32_t RNG_PTR_RVA = 0x48365C, RNG_W_OFF = 0x300, RNG_DRAW_INT = 0x802CF, RNG_DRAW_FLOAT = 0x80328;

static void SetWatch(HANDLE th, uint32_t addr) {
    CONTEXT c{};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(th, &c)) return;
    c.Dr0 = addr;
    c.Dr6 = 0;
    c.Dr7 = addr ? 0x000D0001 : 0;                  // enable DR0, break on write, 4 bytes
    SetThreadContext(th, &c);
}
static DWORD WINAPI ArmThread(LPVOID arg) {         // runs on a helper thread so it can also stop the game's main thread
    uint32_t addr = (uint32_t)(uintptr_t)arg;
    DWORD ids[256]; int n = 0;
    HANDLE sn = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (sn == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te{sizeof te};
    DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId();
    for (BOOL ok = Thread32First(sn, &te); ok && n < 256; ok = Thread32Next(sn, &te))
        if (te.th32OwnerProcessID == pid && te.th32ThreadID != me) ids[n++] = te.th32ThreadID;
    CloseHandle(sn);
    for (int i = 0; i < n; i++) {
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, ids[i]);
        if (!th) continue;
        if (SuspendThread(th) != (DWORD)-1) { SetWatch(th, addr); ResumeThread(th); }
        CloseHandle(th);
    }
    return 0;
}
static void ArmAll(uint32_t addr) {
    HANDLE h = CreateThread(nullptr, 0, ArmThread, (LPVOID)(uintptr_t)addr, 0, nullptr);
    if (h) { WaitForSingleObject(h, 5000); CloseHandle(h); }
    RngWatch = addr;
}
static bool RngLogWanted() {
    return S->rng_log && (S->draw_from == 0 || S->frame + 300 >= S->draw_from);
}
static void RngCheck() {                            // at every frame marker
    uint32_t addr = 0;
    if (RngLogWanted()) {
        uint32_t obj = *(volatile uint32_t*)((uint8_t*)GetModuleHandleW(NULL) + RNG_PTR_RVA);
        if (obj) addr = obj + RNG_W_OFF;
    }
    if (addr != RngWatch) ArmAll(addr);
}
static LONG CALLBACK RngVeh(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = e->ContextRecord;
    if (!(c->Dr6 & 1) || !RngWatch) return EXCEPTION_CONTINUE_SEARCH;
    uint32_t base = (uint32_t)(uintptr_t)GetModuleHandleW(NULL), rva = c->Eip - base;
    RngLogEntry en{};
    en.frame = S->frame;
    en.eip = rva;
    en.w = *(volatile uint32_t*)RngWatch;
    if (rva == RNG_DRAW_INT) {                      // inside the draw function: ebp frame is set up
        en.kind = 1;
        en.range = *(uint32_t*)(c->Ebp + 8);
        en.ret = *(uint32_t*)(c->Ebp + 4) - base;
    } else if (rva == RNG_DRAW_FLOAT) {             // after `push esi`
        en.kind = 2;
        en.ret = *(uint32_t*)(c->Esp + 4) - base;
    } else {                                        // inlined copy: first plausible return address on the stack
        uint32_t* sp = (uint32_t*)c->Esp;
        for (int i = 0; i < 64; i++) {
            uint32_t v = sp[i];
            if (v > base + 6 && v < base + 0x350000) {
                uint8_t* p = (uint8_t*)v;
                if (p[-5] == 0xE8 || p[-6] == 0xFF || p[-2] == 0xFF || p[-3] == 0xFF) { en.ret = v - base; break; }
            }
        }
    }
    uint32_t i = InterlockedIncrement((volatile LONG*)&S->rng_log_n) - 1;
    S->rng_log_buf[i & (RNG_LOG_MAX - 1)] = en;
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}
// ---- worker threads ------------------------------------------------------------
// The game starts short-lived threads for loading work and checks on them a frame
// later. At 1x each finishes inside one 16 ms frame; at 50x a frame lasts ~0.3 ms, so
// they would still be running and the game would see the result several frames later
// (a menu became responsive at a different frame, and scripted input was lost). So every
// frame boundary first waits for the threads started since the previous one.
static HANDLE (WINAPI *R_Ct)(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
static CRITICAL_SECTION ThCs;
static HANDLE Pending[MAXIMUM_WAIT_OBJECTS];
static int    NPending;

// ---- crash log (savestate diagnosis): first fatal exception -> %TEMP%\bscotm_crash.txt ------------
static void XaCrashInfo(void* cb, char* out, int n);     // defined with the voice table
static bool XaIsShim(uintptr_t a);
static bool XaIsRealVoice(uintptr_t a);
static void XaFindInternal(HANDLE f, uintptr_t obj);
static void DumpFirstCreates(HANDLE f);
static void XaDumpEvents(HANDLE f, uint32_t cb, uint32_t realv);
static LONG CALLBACK CrashVeh(PEXCEPTION_POINTERS ep) {
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    if (c != 0xC0000005 && c != 0xC0000094 && c != 0xC000001D && c != 0xC0000096 && c != 0xC00000FD && c != 0xC0000374 && c != 0xC0000409) return EXCEPTION_CONTINUE_SEARCH;
    static volatile LONG once;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
    char path[MAX_PATH + 32];
    GetTempPathA(MAX_PATH, path);
    strcat(path, "bscotm_crash.txt");
    HANDLE f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    CONTEXT* x = ep->ContextRecord;
    char b[512];
    auto put = [&](const char* s) { DWORD w; WriteFile(f, s, (DWORD)strlen(s), &w, nullptr); };
    wsprintfA(b, "code %08X at %08X (exe+%X) tid %lu marker %u\r\n", c, (unsigned)(uintptr_t)ep->ExceptionRecord->ExceptionAddress,
              (unsigned)((uintptr_t)ep->ExceptionRecord->ExceptionAddress - base), GetCurrentThreadId(), S ? S->paused : 0);
    put(b);
    if (c == 0xC0000005 && ep->ExceptionRecord->NumberParameters >= 2) {
        wsprintfA(b, "access %s of %08X\r\n", ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read", (unsigned)ep->ExceptionRecord->ExceptionInformation[1]);
        put(b);
    }
    wsprintfA(b, "eax %08X ebx %08X ecx %08X edx %08X esi %08X edi %08X ebp %08X esp %08X\r\n", (unsigned)x->Eax, (unsigned)x->Ebx,
              (unsigned)x->Ecx, (unsigned)x->Edx, (unsigned)x->Esi, (unsigned)x->Edi, (unsigned)x->Ebp, (unsigned)x->Esp);
    put(b);
    put("stack (exe RVAs marked *):\r\n");
    auto modname = [&](uintptr_t a, char* out) {
        HMODULE hm = nullptr;
        out[0] = 0;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &hm) && hm) {
            char full[MAX_PATH];
            GetModuleFileNameA(hm, full, MAX_PATH);
            const char* n = strrchr(full, '\\');
            wsprintfA(out, "%s+%X", n ? n + 1 : full, (unsigned)(a - (uintptr_t)hm));
        }
    };
    uintptr_t stk[64];
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (void*)x->Esp, stk, sizeof stk, &got)) got = 0;
    for (SIZE_T i = 0; i < got / 4; i++) {
        uintptr_t v = stk[i];
        bool in = v >= base && v < base + 0x800000;
        wsprintfA(b, "  [esp+%02X] %08X%s\r\n", (unsigned)(i * 4), (unsigned)v, in ? " *" : "");
        put(b);
        char mn[300];
        modname(v, mn);
        if (mn[0]) { wsprintfA(b, "           %s\r\n", mn); put(b); }
    }
    {   // code bytes around the fault (the exe is packed on disk, so this is the only way to see its real code)
        uint8_t code[160];
        SIZE_T cg = 0;
        uintptr_t ip = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
        if (ReadProcessMemory(GetCurrentProcess(), (void*)(ip - 96), code, sizeof code, &cg) && cg == sizeof code) {
            wsprintfA(b, "code bytes from %08X (fault at +96):\r\n", (unsigned)(ip - 96));
            put(b);
            for (size_t i = 0; i < sizeof code; i++) { wsprintfA(b, "%02x", code[i]); put(b); }
            put("\r\n");
        }
    }
    {   // what is at ecx (the object the game was calling a method of)?
        uint8_t ob[64];
        SIZE_T og = 0;
        if (ReadProcessMemory(GetCurrentProcess(), (void*)x->Ecx, ob, sizeof ob, &og) && og) {
            wsprintfA(b, "bytes at ecx %08X:\r\n", (unsigned)x->Ecx);
            put(b);
            for (SIZE_T i = 0; i < og; i++) { wsprintfA(b, "%02x", ob[i]); put(b); }
            put("\r\n");
        } else put("ecx not readable\r\n");
        wsprintfA(b, "ecx is a tracked XAudio2 voice object: %s\r\n", XaIsRealVoice(x->Ecx) ? "YES" : "no");
        put(b);
    }
    char xi[1024];
    XaCrashInfo((void*)x->Edx, xi, sizeof xi);
    wsprintfA(b, "callback object (edx) %08X: ", (unsigned)x->Edx);
    put(b);
    put(xi);
    put("\r\n");
    uintptr_t arg0 = 0;
    {
        uintptr_t vo[24];
        SIZE_T g1 = 0, g2 = 0;
        ReadProcessMemory(GetCurrentProcess(), (void*)(x->Ebp + 8), &arg0, 4, &g1);
        wsprintfA(b, "frame arg0 (XAudio2 voice) %08X\r\n", (unsigned)arg0);
        put(b);
        if (g1 == 4 && ReadProcessMemory(GetCurrentProcess(), (void*)arg0, vo, sizeof vo, &g2)) {
            for (SIZE_T i = 0; i < g2 / 4; i++) {
                bool shim = false;
                shim = XaIsShim(vo[i]);
                wsprintfA(b, "  voice+%02X: %08X%s\r\n", (unsigned)(i * 4), (unsigned)vo[i], shim ? "  <- a hook shim" : "");
                put(b);
            }
        }
    }
    XaFindInternal(f, arg0);
    DumpFirstCreates(f);
    XaDumpEvents(f, (uint32_t)x->Edx, (uint32_t)arg0);
    char mn0[300];
    modname((uintptr_t)ep->ExceptionRecord->ExceptionAddress, mn0);
    wsprintfA(b, "fault address module: %s\r\n", mn0[0] ? mn0 : "(none: not inside any module)");
    put(b);
    CloseHandle(f);
    return EXCEPTION_CONTINUE_SEARCH;
}

static HANDLE WINAPI H_Ct(LPSECURITY_ATTRIBUTES a, SIZE_T sz, LPTHREAD_START_ROUTINE fn, LPVOID p, DWORD fl, LPDWORD id) {
    HANDLE h = R_Ct(a, sz, fn, p, fl, id);
    if (h && RngWatch && SuspendThread(h) != (DWORD)-1) { SetWatch(h, RngWatch); ResumeThread(h); }   // RNG log: watch new threads too
    HANDLE d;
    if (h && GateTls != TLS_OUT_OF_INDEXES) GateRegister(h, GetThreadId(h));
    if (h && !(fl & CREATE_SUSPENDED) &&    // the game may close h at once, so keep our own copy
        DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &d, SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
        EnterCriticalSection(&ThCs);
        if (NPending < MAXIMUM_WAIT_OBJECTS) Pending[NPending++] = d; else CloseHandle(d);
        LeaveCriticalSection(&ThCs);
    }
    return h;
}

static void JoinThreads() {
    HANDLE list[MAXIMUM_WAIT_OBJECTS];
    EnterCriticalSection(&ThCs);
    int n = NPending;
    memcpy(list, Pending, n * sizeof(HANDLE));
    NPending = 0;
    LeaveCriticalSection(&ThCs);
    if (!n) return;
    HANDLE wait[MAXIMUM_WAIT_OBJECTS];              // not the ones that sit in a Sleep loop: they are idle, not busy
    int nw = 0;
    for (int i = 0; i < n; i++)
        if (WaitForSingleObject(list[i], 0) != WAIT_OBJECT_0 && !IsSleeper(GetThreadId(list[i]))) wait[nw++] = list[i];
    if (nw) WaitForMultipleObjects(nw, wait, TRUE, 3000);    // bounded, in case a thread never ends
    for (int i = 0; i < n; i++) CloseHandle(list[i]);
}

// ---- value history ----------------------------------------------------------------------------------
// While the host asks for it (Shm::hist_on) every frame marker takes one sample of the player's values
// from game memory (the same pointer chains the editor's Memory window uses, read here without leaving
// the process), so a graph can show every frame, also during fast-forward.
static bool SelfRead(uint32_t addr, void* out, size_t n) {
    SIZE_T g = 0;
    return addr && ReadProcessMemory(GetCurrentProcess(), (void*)(uintptr_t)addr, out, n, &g) && g == n;
}
static void HistSample(uint32_t marker) {
    HistEntry e;
    e.frame = marker;
    for (int i = 0; i < HIST_FIELDS; i++) e.v[i] = i < 3 ? 0xFFFFFFFFu : 0x7FC00000u;
    uint32_t base = (uint32_t)(uintptr_t)GetModuleHandleW(NULL), p = 0;
    // player: *(exe+0x48365C), then +0x08, +0x6C, +0x20, +0x20, +0x08, +0x84 (each dereferenced) gives the struct
    if (SelfRead(base + 0x48365C, &p, 4) && p) {
        static const uint32_t steps[] = {0x08, 0x6C, 0x20, 0x20, 0x08, 0x84};
        bool ok = true;
        for (uint32_t off : steps) if (!SelfRead(p + off, &p, 4) || !p) { ok = false; break; }
        uint8_t blk[0x544];
        if (ok && SelfRead(p, blk, sizeof blk)) {
            e.v[0] = blk[0x3DC];
            memcpy(&e.v[3], blk + 0x1A0, 4);      // X speed
            memcpy(&e.v[4], blk + 0x1A4, 4);      // Y speed
            memcpy(&e.v[5], blk + 0x1AC, 4);      // X
            memcpy(&e.v[6], blk + 0x1B0, 4);      // Y
            memcpy(&e.v[7], blk + 0x53C, 4);      // invisibility
        }
    }
    uint32_t q = 0;                                // weapon points and score: *(exe+0x483660) -> +0x08 -> +0x1E / +0x24
    if (SelfRead(base + 0x483660, &q, 4) && q && SelfRead(q + 0x08, &q, 4) && q) {
        uint8_t ammo;
        uint32_t score;
        if (SelfRead(q + 0x1E, &ammo, 1)) e.v[1] = ammo;
        if (SelfRead(q + 0x24, &score, 4)) e.v[2] = score;
    }
    uint32_t i = S->hist_n;
    S->hist_buf[i & (HIST_MAX - 1)] = e;
    MemoryBarrier();
    S->hist_n = i + 1;
}

// ---- game heap -> arena (savestates) ----------------------------------------------
static LPVOID (WINAPI *R_HeapAlloc)(HANDLE, DWORD, SIZE_T);
static BOOL   (WINAPI *R_HeapFree)(HANDLE, DWORD, LPVOID);
static LPVOID (WINAPI *R_HeapReAlloc)(HANDLE, DWORD, LPVOID, SIZE_T);
static SIZE_T (WINAPI *R_HeapSize)(HANDLE, DWORD, LPCVOID);

static LPVOID WINAPI H_HeapAlloc(HANDLE h, DWORD fl, SIZE_T n) {
    void* p = arena::Alloc(n, (fl & HEAP_ZERO_MEMORY) != 0);
    return p ? p : R_HeapAlloc(h, fl, n);
}

static BOOL WINAPI H_HeapFree(HANDLE h, DWORD fl, LPVOID p) {
    if (!p) return TRUE;
    if (arena::Contains(p)) { arena::Free(p); return TRUE; }
    return R_HeapFree(h, fl, p);
}

static LPVOID WINAPI H_HeapReAlloc(HANDLE h, DWORD fl, LPVOID p, SIZE_T n) {
    if (!p || !arena::Contains(p)) return R_HeapReAlloc(h, fl, p, n);
    size_t old = arena::RequestedSize(p), cap = arena::Capacity(p);
    bool zero = (fl & HEAP_ZERO_MEMORY) != 0;
    if (n <= cap) {
        arena::SetRequested(p, n);
        if (zero && n > old) memset((char*)p + old, 0, n - old);
        return p;
    }
    if (fl & HEAP_REALLOC_IN_PLACE_ONLY) return nullptr;
    void* q = arena::Alloc(n, false);
    if (!q) q = R_HeapAlloc(h, fl & ~HEAP_ZERO_MEMORY, n);
    if (!q) return nullptr;
    memcpy(q, p, old < n ? old : n);
    if (zero && n > old) memset((char*)q + old, 0, n - old);
    arena::Free(p);
    return q;
}

static SIZE_T WINAPI H_HeapSize(HANDLE h, DWORD fl, LPCVOID p) {
    return p && arena::Contains(p) ? arena::RequestedSize(p) : R_HeapSize(h, fl, p);
}

// ---- XAudio2 voices (savestates) -------------------------------------------------
// The game keeps pointers to its XAudio2 source voices, and each voice keeps a pointer to a
// callback object in the game's heap. Putting the heap back therefore needs care:
//   - a voice created after the save must be destroyed before the restore (its callback
//     object is about to disappear), and
//   - a voice the game destroyed after the save must exist again afterwards, because the
//     restored game memory points at it.
// So the game is handed a stand-in object (same layout: vtable pointer first) per source
// voice. Every call is forwarded to the real voice; DestroyVoice is the only one handled here.
// A destroyed voice can then be recreated behind the same stand-in pointer.
struct Proxy { void** vt; void* real; };
struct XaShimT { void** vt; void* owner; };     // what XAudio2 gets as the voice callback: lives in the hook, not the game heap
struct XaVoice {
    Proxy px;
    bool in_use, real_dead, recreatable;
    uint32_t created, destroyed;             // XaSerial values
    uint8_t fmt[128]; uint32_t flags; float maxfreq; void* cb;
    uint32_t nsends; struct { uint32_t flags; void* out; } sends[4];
    XaShimT shim;                            // shim.vt / shim.owner set when the voice is created
    volatile LONG inflight, cb_alive;        // callbacks running through the shim / may the game callback still be called
};
static XaVoice  Voices[1024];
static bool XaIsShim(uintptr_t a) { for (int k = 0; k < 1024; k++) if (a == (uintptr_t)&Voices[k].shim) return true; return false; }
static bool XaIsRealVoice(uintptr_t a) { for (int k = 0; k < 1024; k++) if (Voices[k].px.real && (uintptr_t)Voices[k].px.real == a) return true; return false; }
static uint32_t XaSerial;                    // bumped on every create / destroy / save
static uint32_t SlotSerial[8];               // XaSerial when each slot was saved (0 = empty)
static CRITICAL_SECTION XaCs;
static void*    XaObject;                    // the IXAudio2 the voices came from
typedef HRESULT (__stdcall *PFN_CreateSrc)(void*, void**, const void*, UINT32, float, void*, const void*, const void*);
static PFN_CreateSrc R_CreateSrc;
static HRESULT (WINAPI *R_CoCreate)(const GUID&, LPUNKNOWN, DWORD, const GUID&, LPVOID*);

#define PX_LIST(X) X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) \
    X(15) X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28)
#define PX_DECL(n) extern "C" void px##n() __asm__("_bscotm_px" #n);
#define PX_ADDR(n) (void*)px##n,
#define PX_DEF(n) __asm__(".text\n.globl _bscotm_px" #n "\n_bscotm_px" #n ":\n    lock incl _bscotm_pxcnt+" #n "*4\n" \
    "    mov 4(%esp), %eax\n    mov 4(%eax), %eax\n    mov %eax, 4(%esp)\n    mov (%eax), %eax\n    jmp *" #n "*4(%eax)\n");
extern "C" uint32_t PxCnt[32] __asm__("_bscotm_pxcnt");
uint32_t PxCnt[32];
static volatile uint32_t ShC[8];                       // diagnosis: callbacks received by the shims, by method
PX_LIST(PX_DECL)
PX_LIST(PX_DEF)
static void* ProxyVt[29] = { PX_LIST(PX_ADDR) };

// ---- callback shim: XAudio2 never holds a pointer into the game heap -------------------------
// XAudio2 calls IXAudio2VoiceCallback methods on its own audio thread, at any time. After a restore
// (or just after the game destroyed a voice and freed its callback) such a call lands in memory that
// no longer holds the callback. So XAudio2 is given a shim object per voice; it forwards the call to
// the game's callback only while the voice is alive and no restore is under way. Before the game's
// callback can go away, XaKillCb stops forwarding and waits for the calls in flight.
static volatile LONG XaCbGate;               // 1 = callbacks are dropped (a restore is under way)
static volatile LONG XaOverflow;             // voices handed out untracked because the table was full (should stay 0)
static volatile LONG XaLost;                 // voices that existed at a save but could not be brought back by a restore
static volatile LONG ShimCalls, ShimForwarded;
#define SH_BEGIN XaVoice* v = (XaVoice*)s->owner; InterlockedIncrement(&ShimCalls); InterlockedIncrement(&v->inflight); MemoryBarrier(); \
                 void* cb = v->cb; bool ok = v->cb_alive && !XaCbGate && cb && !(S->features & FEAT_DROPCB);
#define SH_END   if (ok) InterlockedIncrement(&ShimForwarded); InterlockedDecrement(&v->inflight);
#define SH_VT(i) (*(void***)cb)[i]
static void __stdcall Sh0(XaShimT* s, UINT32 n)       { ShC[0]++; SH_BEGIN if (ok) ((void (__stdcall*)(void*, UINT32))SH_VT(0))(cb, n); SH_END }
static void __stdcall Sh1(XaShimT* s)                 { ShC[1]++; SH_BEGIN if (ok) ((void (__stdcall*)(void*))SH_VT(1))(cb); SH_END }
static void __stdcall Sh2(XaShimT* s)                 { ShC[2]++; SH_BEGIN if (ok) ((void (__stdcall*)(void*))SH_VT(2))(cb); SH_END }
static void __stdcall Sh3(XaShimT* s, void* c)        { ShC[3]++; SH_BEGIN if (ok) ((void (__stdcall*)(void*, void*))SH_VT(3))(cb, c); SH_END }
static void __stdcall Sh4(XaShimT* s, void* c)        { ShC[4]++; SH_BEGIN if (ok) ((void (__stdcall*)(void*, void*))SH_VT(4))(cb, c); SH_END }
static void __stdcall Sh5(XaShimT* s, void* c)        { ShC[5]++; SH_BEGIN if (ok) ((void (__stdcall*)(void*, void*))SH_VT(5))(cb, c); SH_END }
static void __stdcall Sh6(XaShimT* s, void* c, HRESULT hr) { ShC[6]++; SH_BEGIN if (ok) ((void (__stdcall*)(void*, void*, HRESULT))SH_VT(6))(cb, c, hr); SH_END }
static void* ShimVt[7] = { (void*)Sh0, (void*)Sh1, (void*)Sh2, (void*)Sh3, (void*)Sh4, (void*)Sh5, (void*)Sh6 };

static void XaWaitIdle(XaVoice* v) {
    for (DWORD t0 = GetTickCount(); v->inflight && GetTickCount() - t0 < 2000;) Sleep(0);
}
static void XaKillCb(XaVoice* v) {           // the game's callback object is about to go away
    v->cb_alive = 0;
    MemoryBarrier();
    XaWaitIdle(v);
}

static bool AnySlot() { for (int i = 0; i < 8; i++) if (SlotSerial[i]) return true; return false; }

static void XaCrashInfo(void* cb, char* out, int n) {
    int found = 0, live = 0;
    out[0] = 0;
    for (int i = 0; i < 1024; i++) {
        XaVoice& v = Voices[i];
        if (!v.in_use) continue;
        live++;
        if (v.cb == cb) {
            found++;
            wsprintfA(out + lstrlenA(out), "voice#%d proxy %08X real %08X created %u destroyed %u real_dead %d recreatable %d; ", i, (unsigned)(uintptr_t)&v.px, (unsigned)(uintptr_t)v.px.real,
                      v.created, v.destroyed, (int)v.real_dead, (int)v.recreatable);
        }
    }
    wsprintfA(out + lstrlenA(out), "%d voices match cb, %d live, XaSerial %u, slot0 serial %u, shim calls %ld forwarded %ld, gate %ld, TABLE OVERFLOWS %ld, LOST VOICES %ld", found, live, XaSerial,
              SlotSerial[0], ShimCalls, ShimForwarded, XaCbGate, XaOverflow, XaLost);
    (void)n;
}

static void XaReap() {                       // forget destroyed voices that no saved slot can bring back
    uint32_t keep = 0xFFFFFFFFu;
    for (int i = 0; i < 8; i++) if (SlotSerial[i] && SlotSerial[i] < keep) keep = SlotSerial[i];
    for (auto& v : Voices) if (v.in_use && v.destroyed && v.destroyed < keep) v.in_use = false;
}

struct XaEvt { uint32_t tick, tid, what, proxy, real, cb, serial, marker; };
static XaEvt XaLog[1024];
static volatile LONG XaLogN;
static void XaEv(uint32_t what, const void* proxy, const void* real, const void* cb) {
    LONG i = InterlockedIncrement(&XaLogN) - 1;
    XaEvt& e = XaLog[i % 1024];
    e.tick = GetTickCount(); e.tid = GetCurrentThreadId(); e.what = what;
    e.proxy = (uint32_t)(uintptr_t)proxy; e.real = (uint32_t)(uintptr_t)real; e.cb = (uint32_t)(uintptr_t)cb;
    e.serial = XaSerial; e.marker = S ? S->paused : 0;
}
static void XaDumpEvents(HANDLE f, uint32_t cb, uint32_t realv) {
    char b[256];
    DWORD w;
    LONG n = XaLogN;
    LONG from = n > 1024 ? n - 1024 : 0;
    static const char* const NAME[] = {"?", "create", "destroy", "destroy-IGNORED", "recreate", "destroy-before-restore", "save", "restore-start", "pause", "after-copy", "", "", "destroy-returned", "", "", "", "", "", "", "", "shim-check", "", "LOST-AT-RESTORE"};
    for (LONG i = from; i < n; i++) {
        const XaEvt& e = XaLog[i % 1024];
        bool mine = e.cb == cb || e.real == realv;
        bool mark = e.what == 6 || e.what == 7 || e.what == 9 || e.what == 22;
        if (!mine && !mark) continue;
        wsprintfA(b, "  evt %4ld t=%u tid=%u marker=%u serial=%u %-22s proxy=%08X real=%08X cb=%08X\r\n", i, e.tick, e.tid, e.marker, e.serial,
                  NAME[e.what < 23 ? e.what : 0], e.proxy, e.real, e.cb);
        WriteFile(f, b, lstrlenA(b), &w, nullptr);
    }
}
static void XaFindInternal(HANDLE f, uintptr_t obj) {        // which tracked voice's object points at `obj`?
    char b[256];
    DWORD w;
    int hits = 0;
    for (int i = 0; i < 1024; i++) {
        XaVoice& v = Voices[i];
        if (!v.px.real) continue;
        for (int k = 0; k < 0x80; k += 4) {
            uintptr_t val;
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), (char*)v.px.real + k, &val, 4, &got) || got != 4) break;
            if (val == obj || (val <= obj && obj < val + 0x400 && val > 0x10000 && (obj - val) % 4 == 0 && k > 0 && false)) {
                wsprintfA(b, "  tracked voice#%d (proxy %08X real %08X) has [real+%02X] = the crashing XAudio2 object; in_use %d dead %d created %u destroyed %u cb_alive %d\r\n",
                          i, (unsigned)(uintptr_t)&v.px, (unsigned)(uintptr_t)v.px.real, k, (int)v.in_use, (int)v.real_dead, v.created, v.destroyed, (int)v.cb_alive);
                WriteFile(f, b, lstrlenA(b), &w, nullptr);
                hits++;
            }
        }
    }
    for (int i = 0; i < 1024; i++) {        // the voice object is `real`; the crashing frame works on its sub-object real + 0x8C
        XaVoice& v = Voices[i];
        if (!v.px.real || (uintptr_t)v.px.real + 0x8C != obj) continue;
        uintptr_t cur = 0;
        SIZE_T got = 0;
        ReadProcessMemory(GetCurrentProcess(), (char*)v.px.real + 0xC4, &cur, 4, &got);
        wsprintfA(b, "  THIS IS tracked voice#%d (proxy %08X real %08X): in_use %d real_dead %d created %u destroyed %u cb_alive %d; stored callback now %08X, our shim %08X, game cb %08X\r\n",
                  i, (unsigned)(uintptr_t)&v.px, (unsigned)(uintptr_t)v.px.real, (int)v.in_use, (int)v.real_dead, v.created, v.destroyed, (int)v.cb_alive,
                  (unsigned)cur, (unsigned)(uintptr_t)&v.shim, (unsigned)(uintptr_t)v.cb);
        WriteFile(f, b, lstrlenA(b), &w, nullptr);
        hits++;
    }
    wsprintfA(b, "  %d tracked voices point at the crashing XAudio2 object\r\n", hits);
    WriteFile(f, b, lstrlenA(b), &w, nullptr);
}
// Layout probe (diagnosis): where does XAudio2 keep the callback pointer it was given? Searches the returned
// voice object and the objects it points to (two levels) for the shim and for the game's callback.
static char ProbeLog[8192];
static int  ProbeLen;
static volatile LONG NProbes;
static void ProbeAdd(const char* s) { int n = lstrlenA(s); if (ProbeLen + n < (int)sizeof ProbeLog - 1) { memcpy(ProbeLog + ProbeLen, s, n); ProbeLen += n; ProbeLog[ProbeLen] = 0; } }
static bool Plausible(uintptr_t p) { return p >= 0x10000 && p < 0x7FFE0000; }
static void ProbeVoice(void* real, void* shim, void* cb) {
    if (InterlockedIncrement(&NProbes) > 4) return;
    char b[200];
    wsprintfA(b, "probe: real %08X shim %08X gamecb %08X\r\n", (unsigned)(uintptr_t)real, (unsigned)(uintptr_t)shim, (unsigned)(uintptr_t)cb);
    ProbeAdd(b);
    static uintptr_t l0[0x80], l1[0x80], l2[0x40];
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), real, l0, sizeof l0, &got)) return;
    for (SIZE_T i = 0; i < got / 4; i++) {
        if (l0[i] == (uintptr_t)shim || l0[i] == (uintptr_t)cb) {
            wsprintfA(b, "  real+%X holds the %s\r\n", (unsigned)(i * 4), l0[i] == (uintptr_t)cb ? "GAME callback" : "shim"); ProbeAdd(b);
        }
        if (i == 0 || !Plausible(l0[i])) continue;
        SIZE_T g1 = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), (void*)l0[i], l1, sizeof l1, &g1)) continue;
        for (SIZE_T j = 0; j < g1 / 4; j++) {
            if (l1[j] == (uintptr_t)shim || l1[j] == (uintptr_t)cb) {
                wsprintfA(b, "  [real+%X]=%08X +%X holds the %s\r\n", (unsigned)(i * 4), (unsigned)l0[i], (unsigned)(j * 4), l1[j] == (uintptr_t)cb ? "GAME callback" : "shim"); ProbeAdd(b);
            }
            if (!Plausible(l1[j]) || j == 0) continue;
            SIZE_T g2 = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), (void*)l1[j], l2, sizeof l2, &g2)) continue;
            for (SIZE_T k = 0; k < g2 / 4; k++)
                if (l2[k] == (uintptr_t)shim || l2[k] == (uintptr_t)cb) {
                    wsprintfA(b, "  [[real+%X]+%X]=%08X +%X holds the %s\r\n", (unsigned)(i * 4), (unsigned)(j * 4), (unsigned)l1[j], (unsigned)(k * 4), l2[k] == (uintptr_t)cb ? "GAME callback" : "shim"); ProbeAdd(b);
                }
        }
    }
}
struct FirstCreate { uint32_t real, vt, f34, f38, shim, cb; };
static FirstCreate FirstCreates[8];
static volatile LONG NFirstCreates;
static void NoteFirstCreate(void* real, void* shim, void* cb) {
    LONG i = InterlockedIncrement(&NFirstCreates) - 1;
    if (i >= 8) return;
    uintptr_t w[16] = {};
    SIZE_T got = 0;
    ReadProcessMemory(GetCurrentProcess(), real, w, sizeof w, &got);
    FirstCreates[i] = { (uint32_t)(uintptr_t)real, (uint32_t)w[0], (uint32_t)w[0x34 / 4], (uint32_t)w[0x38 / 4], (uint32_t)(uintptr_t)shim, (uint32_t)(uintptr_t)cb };
}
static void DumpFirstCreates(HANDLE f) {
    DWORD wp;
    WriteFile(f, ProbeLog, ProbeLen, &wp, nullptr);
    char b[200];
    DWORD w;
    for (LONG i = 0; i < NFirstCreates && i < 8; i++) {
        const FirstCreate& c = FirstCreates[i];
        wsprintfA(b, "  create#%ld real %08X [real]=%08X [+34]=%08X [+38]=%08X  (passed shim %08X, game cb %08X)\r\n", i, c.real, c.vt, c.f34, c.f38, c.shim, c.cb);
        WriteFile(f, b, lstrlenA(b), &w, nullptr);
    }
}
static void __stdcall H_DestroyVoice(Proxy* self) {
    EnterCriticalSection(&XaCs);
    XaVoice* v = (XaVoice*)self;
    if (v >= Voices && v < Voices + 1024 && v->in_use && !v->real_dead) {
        void* real = v->px.real;
        v->real_dead = true;
        uint32_t newest = 0;                 // only a voice that existed when a slot was saved can be needed again by a restore
        for (int i = 0; i < 8; i++) if (SlotSerial[i] > newest) newest = SlotSerial[i];
        if (newest && v->created <= newest) v->destroyed = ++XaSerial; else v->in_use = false;
        XaEv(2, v, real, v->cb);
        XaKillCb(v);
        LeaveCriticalSection(&XaCs);
        ((void (__stdcall*)(void*))(*(void***)real)[18])(real);
        XaEv(12, v, real, v->cb);               // the real DestroyVoice returned
        return;
    }
    XaEv(3, self, nullptr, nullptr);            // ignored: not ours, already dead, or forgotten
    LeaveCriticalSection(&XaCs);
}

static HRESULT __stdcall H_CreateSource(void* self, void** pp, const void* fmt, UINT32 fl, float freq, void* cb,
                                         const void* sends, const void* chain) {
    EnterCriticalSection(&XaCs);
    XaVoice* v = nullptr;
    for (auto& c : Voices) if (!c.in_use) { v = &c; break; }
    if (!v) {                                 // table full: hand out the real one (untracked: a restore cannot protect the game from it)
        XaOverflow++;
        XaEv(31, nullptr, nullptr, cb);
        LeaveCriticalSection(&XaCs);
        return R_CreateSrc(self, pp, fmt, fl, freq, cb, sends, chain);
    }
    memset(v, 0, sizeof *v);
    v->shim.vt = ShimVt;
    v->shim.owner = v;
    v->cb = cb;
    v->cb_alive = 1;
    void* real = nullptr;
    HRESULT hr = R_CreateSrc(self, &real, fmt, fl, freq, cb ? (void*)&v->shim : nullptr, sends, chain);
    if (hr != 0 || !real || !pp) {
        memset(v, 0, sizeof *v);
        LeaveCriticalSection(&XaCs);
        if (pp) *pp = real;
        return hr;
    }
    v->px.vt = ProxyVt;
    v->px.real = real;
    v->in_use = true;
    v->created = ++XaSerial;
    v->flags = fl; v->maxfreq = freq;
    XaEv(1, v, real, cb);
    NoteFirstCreate(real, &v->shim, cb);
    if (cb) ProbeVoice(real, &v->shim, cb);
    const WAVEFORMATEX* wf = (const WAVEFORMATEX*)fmt;
    uint32_t n = wf ? sizeof(WAVEFORMATEX) + wf->cbSize : 0;
    v->recreatable = wf && n <= sizeof v->fmt && !chain;
    if (v->recreatable) memcpy(v->fmt, wf, n);
    if (sends) {                              // XAUDIO2_VOICE_SENDS { SendCount, pSends }
        uint32_t cnt = *(const uint32_t*)sends;
        const uint32_t* d = *(const uint32_t* const*)((const char*)sends + 4);
        if (cnt > 4) v->recreatable = false;
        else { v->nsends = cnt; for (uint32_t i = 0; i < cnt; i++) { v->sends[i].flags = d[i * 2]; v->sends[i].out = (void*)d[i * 2 + 1]; } }
    }
    LeaveCriticalSection(&XaCs);
    *pp = &v->px;
    return hr;
}

static HRESULT WINAPI H_CoCreate(const GUID& clsid, LPUNKNOWN outer, DWORD ctx, const GUID& iid, LPVOID* out) {
    HRESULT hr = R_CoCreate(clsid, outer, ctx, iid, out);
    static const GUID xa27 = {0x5a508685, 0xa254, 0x4fba, {0x9b, 0x82, 0x9a, 0x24, 0xb0, 0x03, 0x06, 0xaf}};
    static const GUID xa27d = {0xdb05ea35, 0x0329, 0x4d4b, {0xa5, 0x3a, 0x6d, 0xea, 0xd0, 0x3d, 0x3d, 0x38}};
    if (out && *out && (clsid.Data1 == 0x5a508685 || clsid.Data1 == 0xdb05ea35)) {      // XAudio2 object created: note it
        char path[MAX_PATH + 32], b[200];
        GetTempPathA(MAX_PATH, path);
        strcat(path, "bscotm_probe.txt");
        HANDLE pf = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
        if (pf != INVALID_HANDLE_VALUE) {
            DWORD w;
            void** vt = *(void***)*out;
            wsprintfA(b, "CoCreate clsid %08X hr %08X obj %08X vtable %08X slot8 %08X patched-already %d\r\n", (unsigned)clsid.Data1, (unsigned)hr, (unsigned)(uintptr_t)*out,
                      (unsigned)(uintptr_t)vt, (unsigned)(uintptr_t)vt[8], (int)(R_CreateSrc != nullptr));
            WriteFile(pf, b, lstrlenA(b), &w, nullptr);
            CloseHandle(pf);
        }
    }
    if (hr == 0 && out && *out && !R_CreateSrc && (!memcmp(&clsid, &xa27, sizeof(GUID)) || !memcmp(&clsid, &xa27d, sizeof(GUID)))) {
        void** vt = *(void***)*out;          // IXAudio2: slot 8 = CreateSourceVoice
        DWORD old;
        if (VirtualProtect(&vt[8], 4, PAGE_READWRITE, &old)) {
            XaObject = *out;
            R_CreateSrc = (PFN_CreateSrc)vt[8];
            vt[8] = (void*)H_CreateSource;
            ProxyVt[18] = (void*)H_DestroyVoice;
            VirtualProtect(&vt[8], 4, old, &old);
        }
    }
    return hr;
}

// StopEngine returns only when no callback into the game is running or can start, so the game
// memory can be copied without the audio thread reading it half way.
static void XaPause()  { if (XaObject) ((void (__stdcall*)(void*))(*(void***)XaObject)[12])(XaObject); }
static void XaResume() { if (XaObject) ((HRESULT (__stdcall*)(void*))(*(void***)XaObject)[11])(XaObject); }

static void XaOnSave(int slot) {
    EnterCriticalSection(&XaCs);
    SlotSerial[slot] = ++XaSerial;
    XaEv(6, nullptr, nullptr, nullptr);
    XaReap();
    LeaveCriticalSection(&XaCs);
}

static void XaBeforeRestore(int slot) {
    EnterCriticalSection(&XaCs);
    uint32_t T = SlotSerial[slot];
    XaEv(7, nullptr, nullptr, nullptr);
    for (auto& v : Voices) {
        if (!v.in_use) continue;
        if (v.created > T) {                  // born after the save: gone before the memory is put back
            XaEv(5, &v, v.px.real, v.cb);
            XaKillCb(&v);
            if (!v.real_dead) ((void (__stdcall*)(void*))(*(void***)v.px.real)[18])(v.px.real);
            v.in_use = false;
        } else if (v.destroyed > T) {         // destroyed after the save: bring it back behind the same pointer
            struct { uint32_t n; void* p; } sl = {v.nsends, v.sends};
            void* nv = nullptr;
            if (v.recreatable && R_CreateSrc && R_CreateSrc(XaObject, &nv, v.fmt, v.flags, v.maxfreq, v.cb ? (void*)&v.shim : nullptr, v.nsends ? &sl : nullptr, nullptr) == 0 && nv) {
                v.px.real = nv;
                v.real_dead = false;
                v.destroyed = 0;
                XaEv(4, &v, nv, v.cb);
            }
        }
    }
    for (auto& v : Voices)                    // buffers the game queued after the save point at game objects that are about to vanish: drop them
        if (v.in_use && !v.real_dead && v.created <= T && v.px.real) {
            XaEv(23, &v, v.px.real, v.cb);
            ((HRESULT (__stdcall*)(void*))(*(void***)v.px.real)[22])(v.px.real);       // IXAudio2SourceVoice::FlushSourceBuffers (completions go to the closed gate)
        }
    for (auto& v : Voices)                    // audit: every voice that existed at the save must be alive again after this
        if (v.in_use && v.created <= T && v.real_dead && !(v.destroyed && v.destroyed <= T)) {
            XaEv(22, &v, (void*)(uintptr_t)(v.recreatable ? 1 : 0), v.cb);           // real column: 1 = was recreatable but recreation failed, 0 = not recreatable
            XaLost++;
        }
    LeaveCriticalSection(&XaCs);
}

// ---- savestate commands (handled while the game is held at a marker) ---------------
// After the memory is put back, this fixes up the hook's own state and the game re-enters the
// poll call from its first instruction, so the marker is handled again and the game freezes
static int64_t RestoreVirt;
static bool RestoreVirtOn;
// at the same marker it was saved at.
static void SnapAfterCopy(const snap::Slot& sn) {
    Shm* s = S;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    VirtBase = sn.virt;
    RestoreVirt = sn.virt; RestoreVirtOn = true;     // the freeze that follows must not let time run on (it would shift the 1/60 s grid)
    LockBase = sn.aux[0]; LockServed = sn.aux[1];     // the frame-locked clock continues from the saved moment
    { LARGE_INTEGER lq; R_Qpc(&lq); LockMarkReal = lq.QuadPart; }
    RealBase = q.QuadPart;
    CurSpeed = 1000;
    s->speed_milli = 1000;
    s->frame = sn.frame_before;
    s->advance = 0;
    s->hold = 1;
    s->paused = 0;
    NPending = 0;
    XaEv(9, nullptr, nullptr, nullptr);
    for (auto& v : Voices)                  // the game's memory is the saved one again: its callbacks are valid for the voices it knows
        if (v.in_use && !v.real_dead) { v.cb_alive = 1; v.inflight = 0; }
    MemoryBarrier();
    XaCbGate = 0;
    XaResume();
    s->snap_result = 1;
    MemoryBarrier();
    s->snap_cmd = 0;
}

static void WriteThreadInfo() {              // diagnosis: where every other thread was stopped at the last save -> %TEMP%\bscotm_threads.txt
    char path[MAX_PATH + 32], b[400];
    GetTempPathA(MAX_PATH, path);
    strcat(path, "bscotm_threads.txt");
    HANDLE f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w;
    for (int i = 0; i < snap::NInfo; i++) {
        const snap::ThreadInfo& t = snap::Info[i];
        HMODULE hm = nullptr;
        char mod[MAX_PATH] = "?", full[MAX_PATH];
        if (t.eip && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)t.eip, &hm) && hm) {
            GetModuleFileNameA(hm, full, MAX_PATH);
            const char* n = strrchr(full, '\\');
            wsprintfA(mod, "%s+%X", n ? n + 1 : full, (unsigned)(t.eip - (uintptr_t)hm));
        }
        char smod[MAX_PATH] = "?";
        HMODULE hs = nullptr;
        if (t.start && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)t.start, &hs) && hs) {
            GetModuleFileNameA(hs, full, MAX_PATH);
            const char* n = strrchr(full, '\\');
            wsprintfA(smod, "%s+%X", n ? n + 1 : full, (unsigned)(t.start - (uintptr_t)hs));
        }
        wsprintfA(b, "tid %5lu eip %08X esp %08X %s waiting in %s; started at %s\r\n", t.tid, t.eip, t.esp, t.tracked ? "TRACKED  " : "untracked", mod, smod);
        WriteFile(f, b, lstrlenA(b), &w, nullptr);
    }
    for (int i = 0; i < 16 && SleepSites[i].ret; i++) {
        wsprintfA(b, "  tracked-thread Sleep(%u) called from exe+%X : %u times\r\n", SleepSites[i].ms, (unsigned)(SleepSites[i].ret - (uint32_t)(uintptr_t)GetModuleHandleW(nullptr)), SleepSites[i].n);
        WriteFile(f, b, lstrlenA(b), &w, nullptr);
    }
    CloseHandle(f);
}
static void HandleSnap(Shm* s, uint32_t f, int64_t frozen) {
    uint32_t cmd = s->snap_cmd, slot = s->snap_slot;
    if (slot >= (uint32_t)snap::MAX_SLOTS || !(s->features & FEAT_ARENA_OK)) {
        s->snap_result = 2;
    } else if (cmd == 1) {
        DWORD hp0 = GetTickCount();
        XaPause();
        DWORD hp1 = GetTickCount();
        snap::Aux[0] = LockBase; snap::Aux[1] = LockServed;
        bool ok = snap::Save((int)slot, frozen, f - 1);
        DWORD hp2 = GetTickCount();
        if (ok) WriteThreadInfo();
        XaResume();
        s->snap_time[0] = hp1 - hp0; s->snap_time[6] = hp2 - hp1; s->snap_time[7] = GetTickCount() - hp2;
        if (ok) XaOnSave((int)slot);
        s->snap_frame[slot] = ok ? f : 0;
        s->snap_result = ok ? 1 : 2;
    } else if (cmd == 3) {                       // quiet check: can the game's threads be stopped at once?
        DWORD t0 = GetTickCount();
        bool ok = ParkAll();
        DWORD dt = GetTickCount() - t0;
        UnparkAll();
        s->snap_result = (ok && dt <= 20) ? 1 : 2;
    } else if (cmd == 2 && snap::Slots[slot].valid) {
        ParkAll();                  // game worker threads must be idle before voices are destroyed: one may be using a voice we are about to kill
        XaCbGate = 1;               // from here on no callback reaches the game until the memory is back
        MemoryBarrier();
        for (auto& v : Voices) if (v.in_use) XaWaitIdle(&v);
        XaBeforeRestore((int)slot);
        XaPause();
        snap::Restore(&snap::Slots[slot], SnapAfterCopy);      // does not return
    } else {
        s->snap_result = 2;
    }
    MemoryBarrier();
    s->snap_cmd = 0;
}

// ---- frame advance: block the game thread at a frame marker ----------------------
// The game is stopped before frame f reads its input, so the host can still edit
// keys[f - 1]. The virtual clock is frozen meanwhile; otherwise the limiter would
// see the whole pause as elapsed time and run a burst of catch-up frames.
static void Hold(Shm* s, uint32_t f) {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    int64_t frozen = VNow(q.QuadPart);
    if (RestoreVirtOn) { frozen = RestoreVirt; RestoreVirtOn = false; }     // right after a load the game is frozen at exactly the saved time
    s->paused = f;
    while (s->hold) {
        if (s->snap_cmd) HandleSnap(s, f, frozen);
        uint32_t a = s->advance;        // atomic: the host may reset it to abort a batch
        if (a && InterlockedCompareExchange((volatile LONG*)&s->advance, (LONG)(a - 1), (LONG)a) == (LONG)a) break;
        Sleep(1);
    }
    QueryPerformanceCounter(&q);
    EnterCriticalSection(&Cs);
    VirtBase = frozen;
    RealBase = q.QuadPart;
    LockMarkReal = q.QuadPart;               // the wait for the host is not a stall
    LeaveCriticalSection(&Cs);
    if (s->mode == M_RECORD) Cur = 0;       // recording starts from a clean frame
    s->paused = 0;
}

// ---- input -----------------------------------------------------------------
static void Marker(Shm* s) {
    JoinThreads();
    uint32_t f = s->frame + 1;
    s->frame = f;
    { LARGE_INTEGER qq; R_Qpc(&qq); LockAtMarker(f, VNow(qq.QuadPart)); }     // frame-locked clock: starts at the first marker
    RngCheck();
    MarkerTid = (LONG)GetCurrentThreadId();
    if (f < 12288) for (int i = 0; i < 4; i++) s->thr_log[i][f] = (uint16_t)ThrCnt[i];
    if (s->lockstep) LockstepRelease(s->lockstep);
    if (s->quant_hz) {      // diagnosis: how many grid steps passed since the previous marker
        static int64_t prevq;
        LARGE_INTEGER qq; R_Qpc(&qq);
        int64_t vn = VNow(qq.QuadPart), step = Freq / s->quant_hz, cur = vn < V0 ? vn : V0 + (vn - V0) / step * step;     // the raw grid time, whatever the clamp serves
        if (prevq && cur >= prevq) {
            uint32_t d = (uint32_t)((cur - prevq) / step);
            s->gap_hist[d > 4 ? 4 : d]++;
            if (d != 2 && f > 3000 && s->gap_n < 64) s->gap_log[s->gap_n++] = f << 4 | (d > 15 ? 15 : d);
        }
        prevq = cur;
    }
    memcpy((void*)s->px_cnt, PxCnt, sizeof PxCnt);
    memcpy((void*)s->sh_cnt, (const void*)ShC, sizeof ShC);
    if (s->hist_on) HistSample(f);
    if (s->mode == M_RECORD) {
        if (s->armed && s->rec_count < MAX_FRAMES) s->keys[s->rec_count++] = (uint16_t)Cur;
        s->armed = 1;
        Cur = 0;
    } else if (s->mode == M_PLAY && f > s->stop_at) {
        s->speed_milli = 1000;          // live play / recording is always real time
        if (s->then_record) {
            Cur = 0;
            s->armed = 1;
            s->rec_count = s->stop_at;
            s->status |= ST_RESUMED;
            s->mode = M_RECORD;
        } else {
            s->status |= ST_PLAY_END;
            s->mode = M_IDLE;
        }
    }
    if (s->hold_at && f == s->hold_at) {
        s->hold_at = 0;
        s->speed_milli = 1000;          // stepping is always real time
        s->hold = 1;
    }
    UpdateCmdSkip();                    // the game queues this frame's drawing right after the poll
    if (s->hold) { SetCmdSkip(false); Hold(s, f); UpdateCmdSkip(); }
}

static __attribute__((noinline, used)) SHORT WINAPI H_Gaks(int vk) __asm__("_bscotm_gaks");
static __attribute__((noinline, used)) SHORT WINAPI H_Gaks(int vk) {
    if ((uintptr_t)__builtin_return_address(0) != PollRet) return R_Gaks(vk);
    Shm* s = S;
    s->polls++;
    if (vk == 0) {                              // table index 0 = frame boundary
        Focused = !GameWnd || GetForegroundWindow() == GameWnd;
        Marker(s);
        return 0;
    }
    uint32_t b = ((unsigned)vk > 255) ? 0xFF : BitOf[vk];
    if (s->mode == M_PLAY) {                    // scripted: never ask Windows (81 system calls a frame)
        if (b == 0xFF) return 0;
        uint32_t f = s->frame;
        return ((s->keys[f ? f - 1 : 0] >> b) & 1) ? (SHORT)0x8000 : (SHORT)0;
    }
    SHORT r = R_Gaks(vk);
    if (!Focused) return 0;                     // live keys only count in the focused game
    if (b != 0xFF && s->mode == M_RECORD && (r & 0x8000)) Cur |= 1u << b;
    return r;
}

// ---- IAT patching ------------------------------------------------------------
// Diagnosis: who ends the game process, and from where? -> %TEMP%\bscotm_exit.txt
static VOID (WINAPI *R_ExitProcess)(UINT);
static BOOL (WINAPI *R_TermProcess)(HANDLE, UINT);
static void LogExit(const char* what, UINT code, HANDLE target) {
    char path[MAX_PATH + 32], b[300];
    GetTempPathA(MAX_PATH, path);
    strcat(path, "bscotm_exit.txt");
    HANDLE f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w;
    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    uintptr_t* sp = (uintptr_t*)__builtin_frame_address(0);
    wsprintfA(b, "%s(code %u, target %p) tid %lu marker %u virt-locked %d\r\n", what, code, (void*)target, GetCurrentThreadId(), S ? S->frame : 0, (int)LockOn);
    WriteFile(f, b, lstrlenA(b), &w, nullptr);
    for (int i = 0; i < 48; i++) {
        uintptr_t v = sp[i];
        if (v >= base && v < base + 0x800000) { wsprintfA(b, "  [sp+%02X] exe+%X\r\n", i * 4, (unsigned)(v - base)); WriteFile(f, b, lstrlenA(b), &w, nullptr); }
    }
    CloseHandle(f);
}
static VOID WINAPI H_ExitProcess(UINT c) { LogExit("ExitProcess", c, GetCurrentProcess()); R_ExitProcess(c); }
static BOOL WINAPI H_TermProcess(HANDLE h, UINT c) { if (h == GetCurrentProcess() || GetProcessId(h) == GetCurrentProcessId()) LogExit("TerminateProcess", c, h); return R_TermProcess(h, c); }
static bool PatchIat(HMODULE m, const char* dll, const char* fn, void* hook, void* real_out) {
    BYTE* base = (BYTE*)m;
    auto nt  = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp((char*)(base + d->Name), dll) != 0 || !d->OriginalFirstThunk) continue;
        auto names = (IMAGE_THUNK_DATA32*)(base + d->OriginalFirstThunk);
        auto slots = (IMAGE_THUNK_DATA32*)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (names->u1.Ordinal & 0x80000000u) continue;
            auto ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((char*)ibn->Name, fn) != 0) continue;
            DWORD old;
            if (!VirtualProtect(&slots->u1.Function, 4, PAGE_READWRITE, &old)) return false;
            *(void**)real_out = (void*)(uintptr_t)slots->u1.Function;
            slots->u1.Function = (DWORD)(uintptr_t)hook;
            VirtualProtect(&slots->u1.Function, 4, old, &old);
            return true;
        }
    }
    return false;
}

static void Init() {
    HANDLE map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, SHM_NAME);
    if (!map) return;
    S = (Shm*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!S || S->magic != SHM_MAGIC) return;
    memset(BitOf, 0xFF, sizeof BitOf);
    for (int i = 0; i < NUM_KEYS; i++) BitOf[KEYS[i].vk] = (uint8_t)i;
    HMODULE exe = GetModuleHandleW(NULL);
    PollRet = (uintptr_t)exe + POLL_RET_RVA;

    snap::PollSite = PollRet;
    bool ok = PatchIat(exe, "user32.dll", "GetAsyncKeyState", (void*)snap::PollStub, &R_Gaks);
    if (S->features & FEAT_SAVESTATE) {
        if (arena::Init()) {
            snap::FindSections(exe);
            GateTls = TlsAlloc();                   // the gate is only used by savestates
            AddVectoredExceptionHandler(1, CrashVeh);
            snap::ParkHook = ParkAll;
            snap::UnparkHook = UnparkAll;
            snap::GameTids = GameTidList;
            snap::Diag = S->snap_diag;
            snap::Stats = S->snap_stats;
            snap::Times = S->snap_time;
            PatchIat(exe, "kernel32.dll", "HeapAlloc", (void*)H_HeapAlloc, &R_HeapAlloc);
            PatchIat(exe, "kernel32.dll", "HeapFree", (void*)H_HeapFree, &R_HeapFree);
            PatchIat(exe, "kernel32.dll", "HeapReAlloc", (void*)H_HeapReAlloc, &R_HeapReAlloc);
            PatchIat(exe, "kernel32.dll", "HeapSize", (void*)H_HeapSize, &R_HeapSize);
            InitializeCriticalSection(&XaCs);
            PatchIat(exe, "ole32.dll", "CoCreateInstance", (void*)H_CoCreate, &R_CoCreate);
            S->features |= FEAT_ARENA_OK;
        } else {
            S->features |= FEAT_ARENA_FAIL;
        }
    }

    // Clock hooks are optional: a missing import just means that clock stays real.
    InitializeCriticalSection(&Cs);
    InitializeCriticalSection(&ThCs);
    LsSem = CreateSemaphoreW(nullptr, 0, 1000, nullptr);
    PatchIat(exe, "kernel32.dll", "CreateThread", (void*)H_Ct, &R_Ct);
    LARGE_INTEGER f, q;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q);
    Freq = f.QuadPart;
    RealBase = VirtBase = V0 = q.QuadPart;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    Ft0 = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    HMODULE wm = GetModuleHandleW(L"winmm.dll");   // the game imports it; no LoadLibrary under the loader lock
    T0ms = wm ? ((DWORD (WINAPI*)(void))GetProcAddress(wm, "timeGetTime"))() : 0;
    if (PatchIat(exe, "kernel32.dll", "QueryPerformanceCounter", (void*)H_Qpc, &R_Qpc) && R_Qpc) {
        PatchIat(exe, "winmm.dll", "timeGetTime", (void*)H_Tgt, &R_Tgt);
        PatchIat(exe, "kernel32.dll", "GetSystemTimeAsFileTime", (void*)H_Ft, &R_Ft);
        PatchIat(exe, "kernel32.dll", "Sleep", (void*)H_Sleep, &R_Sleep);
        PatchIat(exe, "kernel32.dll", "ExitProcess", (void*)H_ExitProcess, &R_ExitProcess);
        PatchIat(exe, "kernel32.dll", "TerminateProcess", (void*)H_TermProcess, &R_TermProcess);
        PatchIat(exe, "kernel32.dll", "WaitForSingleObject", (void*)H_Wait, &R_Wait);
        PatchIat(exe, "kernel32.dll", "WaitForSingleObjectEx", (void*)H_WaitEx, &R_WaitEx);
    }
    PatchIat(exe, "kernel32.dll", "GetProcAddress", (void*)H_Gpa, &R_Gpa);
    AddVectoredExceptionHandler(1, RngVeh);
    PatchIat(exe, "user32.dll", "CreateWindowExA", (void*)H_Cwe, &R_Cwe);
    PatchIat(exe, "d3d9.dll", "Direct3DCreate9", (void*)H_D3dCreate, &R_D3dCreate);
    S->status |= ok ? ST_HOOKED : ST_HOOK_FAIL;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID) {
    if (why == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); Init(); }
    return TRUE;
}
