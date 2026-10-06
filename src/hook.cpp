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

static void WINAPI H_Ft(FILETIME* f) {
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

static HRESULT WINAPI H_CreateDevice(void* self, UINT ad, UINT type, HWND wnd, DWORD fl, DWORD* pp, void** out) {
    // D3DPRESENT_PARAMETERS: PresentationInterval is the 14th DWORD.
    if ((S->speed_mask & SPEED_NOVSYNC) && pp) pp[13] = 0x80000000u;   // D3DPRESENT_INTERVAL_IMMEDIATE
    return R_CreateDevice(self, ad, type, wnd, fl, pp, out);
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
        if (s->advance) { s->advance--; break; }
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
    SHORT r = R_Gaks(vk);
    if ((uintptr_t)__builtin_return_address(0) != PollRet) return r;
    Shm* s = S;
    s->polls++;
    if (vk == 0) {                              // table index 0 = frame boundary
        Focused = !GameWnd || GetForegroundWindow() == GameWnd;
        Marker(s);
        return r;
    }
    uint32_t b = ((unsigned)vk > 255) ? 0xFF : BitOf[vk];
    if (b != 0xFF && s->mode == M_PLAY) {
        uint32_t f = s->frame;
        return ((s->keys[f ? f - 1 : 0] >> b) & 1) ? (SHORT)0x8000 : (SHORT)0;
    }
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
