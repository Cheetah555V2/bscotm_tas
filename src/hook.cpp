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

static BOOL WINAPI H_Qpc(LARGE_INTEGER* o) {
    BOOL r = R_Qpc(o);
    if (r && (S->speed_mask & SPEED_QPC)) o->QuadPart = VNow(o->QuadPart);
    return r;
}

static DWORD WINAPI H_Tgt() {
    DWORD r = R_Tgt();
    if (!(S->speed_mask & SPEED_MMTIME)) return r;
    LARGE_INTEGER q;
    R_Qpc(&q);
    return T0ms + (DWORD)((VNow(q.QuadPart) - V0) * 1000 / Freq);
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
    uint64_t t = Ft0 + (uint64_t)((VNow(q.QuadPart) - V0) * 10000000 / Freq);
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
static void  WINAPI H_Sleep(DWORD ms) { if (ms) NoteSleep(); R_Sleep(Shorten(ms)); }
static DWORD WINAPI H_Wait(HANDLE h, DWORD ms) { return R_Wait(h, Shorten(ms)); }
static DWORD WINAPI H_WaitEx(HANDLE h, DWORD ms, BOOL a) { return R_WaitEx(h, Shorten(ms), a); }

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
    if (hr == 0 && out && *out) PatchDevice(*out);
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

static HANDLE WINAPI H_Ct(LPSECURITY_ATTRIBUTES a, SIZE_T sz, LPTHREAD_START_ROUTINE fn, LPVOID p, DWORD fl, LPDWORD id) {
    HANDLE h = R_Ct(a, sz, fn, p, fl, id);
    if (h && RngWatch && SuspendThread(h) != (DWORD)-1) { SetWatch(h, RngWatch); ResumeThread(h); }   // RNG log: watch new threads too
    HANDLE d;
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

// ---- frame advance: block the game thread at a frame marker ----------------------
// The game is stopped before frame f reads its input, so the host can still edit
// keys[f - 1]. The virtual clock is frozen meanwhile; otherwise the limiter would
// see the whole pause as elapsed time and run a burst of catch-up frames.
static void Hold(Shm* s, uint32_t f) {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    int64_t frozen = VNow(q.QuadPart);
    s->paused = f;
    while (s->hold) {
        uint32_t a = s->advance;        // atomic: the host may reset it to abort a batch
        if (a && InterlockedCompareExchange((volatile LONG*)&s->advance, (LONG)(a - 1), (LONG)a) == (LONG)a) break;
        Sleep(1);
    }
    QueryPerformanceCounter(&q);
    EnterCriticalSection(&Cs);
    VirtBase = frozen;
    RealBase = q.QuadPart;
    LeaveCriticalSection(&Cs);
    if (s->mode == M_RECORD) Cur = 0;       // recording starts from a clean frame
    s->paused = 0;
}

// ---- input -----------------------------------------------------------------
static void Marker(Shm* s) {
    JoinThreads();
    uint32_t f = s->frame + 1;
    s->frame = f;
    RngCheck();
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

static __attribute__((noinline)) SHORT WINAPI H_Gaks(int vk) {
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

    bool ok = PatchIat(exe, "user32.dll", "GetAsyncKeyState", (void*)H_Gaks, &R_Gaks);

    // Clock hooks are optional: a missing import just means that clock stays real.
    InitializeCriticalSection(&Cs);
    InitializeCriticalSection(&ThCs);
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
