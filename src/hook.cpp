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
#include "common.h"
#include <stdlib.h>
#include <stdio.h>
#include "fixedheap.h"

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

static void NoteCaller(uint32_t src, void* ret) {
    uint32_t n = S->ncallers;
    for (uint32_t i = 0; i < n && i < 64; i++)
        if (S->callers[i].ret == (uint32_t)(uintptr_t)ret && S->callers[i].src == src) { InterlockedIncrement((volatile LONG*)&S->callers[i].count); return; }
    if (n < 64) { S->callers[n].src = src; S->callers[n].ret = (uint32_t)(uintptr_t)ret; S->callers[n].count = 1; S->ncallers = n + 1; }
}
// Experiment: the game resolves some kernel32 functions at run time (GetProcAddress), bypassing our import
// patches. Log what it asks for, and hand out our own clock for the precise-time call.
static FARPROC (WINAPI *R_Gpa)(HMODULE, LPCSTR);
static void (WINAPI *R_FtPrecise)(FILETIME*);
static void (WINAPI *R_FtDyn)(FILETIME*);
static void WINAPI H_FtDyn(FILETIME* f) {
    static const char* fixedEnv = getenv("BSCOTM_FIXTIME");
    if (fixedEnv) {
        uint64_t t = (uint64_t)strtoull(fixedEnv, nullptr, 10) * 10000000ull + 116444736000000000ull;
        f->dwLowDateTime = (DWORD)t; f->dwHighDateTime = (DWORD)(t >> 32);
        return;
    }
    R_FtDyn(f);
}
static void WINAPI H_FtPrecise(FILETIME* f) {
    static const char* fixedEnv = getenv("BSCOTM_FIXTIME");
    if (fixedEnv) {
        uint64_t t = (uint64_t)strtoull(fixedEnv, nullptr, 10) * 10000000ull + 116444736000000000ull;
        f->dwLowDateTime = (DWORD)t; f->dwHighDateTime = (DWORD)(t >> 32);
        return;
    }
    R_FtPrecise(f);
}
static FARPROC WINAPI H_Gpa(HMODULE m, LPCSTR name) {
    FARPROC p = R_Gpa(m, name);
    if (!((uintptr_t)name >> 16)) return p;                 // ordinal
    if (getenv("BSCOTM_LOGGPA")) { FILE* lf = fopen("C:/Users/cheetah/AppData/Local/Temp/claude/D--CodeFile-bscotmTAStool-bscotm-tas/896c3b4d-f788-46a5-8869-a231c49cc7fe/scratchpad/gpa.log", "a"); if (lf) { fprintf(lf, "%s\n", name); fclose(lf); } }
    if (p && !_stricmp(name, "GetSystemTimePreciseAsFileTime")) { R_FtPrecise = (void (WINAPI*)(FILETIME*))p; return (FARPROC)H_FtPrecise; }
    if (p && !_stricmp(name, "GetSystemTimeAsFileTime")) { R_FtDyn = (void (WINAPI*)(FILETIME*))p; return (FARPROC)H_FtDyn; }
    return p;
}
static DWORD (WINAPI *R_Pid)(void);
static DWORD WINAPI H_Pid() { return (S->speed_mask & SPEED_FIXPID) ? 0x1234 : R_Pid(); }
static BOOL WINAPI H_Qpc(LARGE_INTEGER* o) {
    NoteCaller(1, __builtin_return_address(0));
    if (S->speed_mask & SPEED_DETCLOCK) {            // deterministic clock: depends only on how often it is read
        static int64_t det;
        EnterCriticalSection(&Cs);
        if (!det) det = Freq * 1000;
        det += Freq / 3840;                          // 1/64 of a 60 fps frame per call
        o->QuadPart = det;
        LeaveCriticalSection(&Cs);
        return TRUE;
    }
    BOOL r = R_Qpc(o);
    if (r && (S->speed_mask & SPEED_QPC)) o->QuadPart = VNow(o->QuadPart);
    return r;
}

static DWORD WINAPI H_Tgt() {
    NoteCaller(2, __builtin_return_address(0));
    DWORD r = R_Tgt();
    if (!(S->speed_mask & SPEED_MMTIME)) return r;
    LARGE_INTEGER q;
    R_Qpc(&q);
    return T0ms + (DWORD)((VNow(q.QuadPart) - V0) * 1000 / Freq);
}

static void WINAPI H_Ft(FILETIME* f) {
    NoteCaller(3, __builtin_return_address(0));
    static const char* fixedEnv = getenv("BSCOTM_FIXTIME");      // experiment: the game sees this Unix time (seconds), frozen
    if (fixedEnv) {
        uint64_t t = (uint64_t)strtoull(fixedEnv, nullptr, 10) * 10000000ull + 116444736000000000ull;
        f->dwLowDateTime = (DWORD)t; f->dwHighDateTime = (DWORD)(t >> 32);
        return;
    }
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

static void  WINAPI H_Sleep(DWORD ms) { R_Sleep(Shorten(ms)); }
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
    static const bool serial = getenv("BSCOTM_SERIAL") != nullptr;      // experiment: run each worker to completion first
    if (serial && h && !(fl & CREATE_SUSPENDED)) WaitForSingleObject(h, 3000);
    HANDLE d;
    if (h && !(fl & CREATE_SUSPENDED) &&    // the game may close h at once, so keep our own copy
        DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &d, SYNCHRONIZE, FALSE, 0)) {
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
    WaitForMultipleObjects(n, list, TRUE, 3000);    // bounded, in case a thread never ends
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

// ---- tracing -----------------------------------------------------------------------
// The host can put a hardware execute breakpoint on any game address (no code is modified).
// Each hit is logged into the shared block and execution carries on.
static LONG CALLBACK TraceVeh(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = e->ContextRecord;
    if (!(c->Dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;       // not one of ours
    uint32_t i = S->trace_n++ & 8191;
    S->trace[i][0] = S->frame;
    S->trace[i][1] = c->Eip;                // execute breakpoint: the traced instruction; data write: the one after it
    S->trace[i][2] = c->Eax; S->trace[i][3] = c->Ecx; S->trace[i][4] = c->Edx;
    S->trace[i][5] = c->Ebx; S->trace[i][6] = c->Esi; S->trace[i][7] = c->Edi;
    S->trace[i][8] = *(uint32_t*)c->Esp; S->trace[i][9] = *(uint32_t*)(c->Esp + 4);
    if (S->trace_addr && c->Eip == S->trace_addr) c->EFlags |= 0x10000;    // resume flag for execute breakpoints
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Allocation log (experiment): who allocates what during a window of frames. Each entry:
// frame, size, thread id, then up to 7 plausible return addresses found on the stack.
static void AllocLogImpl(size_t n) {
    if (S->trace_addr != 1 || S->frame < S->trace_lo || S->frame > S->trace_hi) return;
    uint32_t i = S->trace_n++ & 8191;
    uint32_t base = (uint32_t)(uintptr_t)GetModuleHandleW(NULL);
    S->trace[i][0] = S->frame; S->trace[i][1] = (uint32_t)n; S->trace[i][2] = GetCurrentThreadId();
    for (int k = 3; k < 32; k++) S->trace[i][k] = 0;
    uint32_t* sp = (uint32_t*)__builtin_frame_address(0); int k = 3;
    for (int w = 0; w < 600 && k < 32; w++) {
        uint32_t v = sp[w];
        if (v > base + 5 && v < base + 0x350000 && (*(uint8_t*)(v - 5) == 0xE8 || *(uint8_t*)(v - 6) == 0xFF || *(uint8_t*)(v - 2) == 0xFF || *(uint8_t*)(v - 3) == 0xFF))
            S->trace[i][k++] = v - base;
    }
}

// ---- input -----------------------------------------------------------------
static void Marker(Shm* s) {
    JoinThreads();
    uint32_t f = s->frame + 1;
    s->frame = f;
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
    if (s->hold) Hold(s, f);
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

    AddVectoredExceptionHandler(1, TraceVeh);
    PatchIat(GetModuleHandleW(NULL), "kernel32.dll", "GetProcAddress", (void*)H_Gpa, &R_Gpa);
    AllocLog = AllocLogImpl;
    if (getenv("BSCOTM_FIXHEAP") && arena::Init()) {     // experiment: the game's heap at a fixed address
        HMODULE ex = GetModuleHandleW(NULL);
        PatchIat(ex, "kernel32.dll", "HeapAlloc", (void*)H_HeapAlloc, &R_HeapAlloc);
        PatchIat(ex, "kernel32.dll", "HeapFree", (void*)H_HeapFree, &R_HeapFree);
        PatchIat(ex, "kernel32.dll", "HeapReAlloc", (void*)H_HeapReAlloc, &R_HeapReAlloc);
        PatchIat(ex, "kernel32.dll", "HeapSize", (void*)H_HeapSize, &R_HeapSize);
    }
    PatchIat(GetModuleHandleW(NULL), "kernel32.dll", "GetCurrentProcessId", (void*)H_Pid, &R_Pid);
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
    PatchIat(exe, "user32.dll", "CreateWindowExA", (void*)H_Cwe, &R_Cwe);
    PatchIat(exe, "d3d9.dll", "Direct3DCreate9", (void*)H_D3dCreate, &R_D3dCreate);
    S->status |= ok ? ST_HOOKED : ST_HOOK_FAIL;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID) {
    if (why == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); Init(); }
    return TRUE;
}
