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

// ---- idle gate: lets a savestate stop the game's own threads at a safe point ------------
// A background thread of the game (it loops on Sleep(8)) runs game code and takes the game's
// locks. Copying or rewriting memory under it would leave a lock half taken. So these
// threads are tracked, count as idle while inside a Sleep/Wait, and when the gate is closed
// they park on the way out of the call. A savestate proceeds only once all of them are idle.
struct GThread { DWORD tid; HANDLE h; volatile LONG idle; };
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
static inline void GateEnter(GThread* g) { if (g) { g->idle = 1; MemoryBarrier(); } }
static void GateLeave(GThread* g) {          // about to run game code again
    if (!g) return;
    for (;;) {
        g->idle = 0;
        MemoryBarrier();
        if (!GateClosed) return;
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
    for (DWORD t0 = GetTickCount(); GetTickCount() - t0 < 3000;) {
        bool all = true;
        for (LONG i = 0; i < NGT; i++)
            if (!GT[i].idle && WaitForSingleObject(GT[i].h, 0) != WAIT_OBJECT_0) all = false;
        if (all) return true;
        Sleep(0);
    }
    return false;
}
static void UnparkAll() { GateClosed = 0; MemoryBarrier(); }

static void  WINAPI H_Sleep(DWORD ms) {
    GThread* g = GateMe();
    GateEnter(g);
    R_Sleep(Shorten(ms));
    GateLeave(g);
}
static DWORD WINAPI H_Wait(HANDLE h, DWORD ms) {
    GThread* g = GateMe();
    GateEnter(g);
    DWORD r = R_Wait(h, Shorten(ms));
    GateLeave(g);
    return r;
}
static DWORD WINAPI H_WaitEx(HANDLE h, DWORD ms, BOOL a) {
    GThread* g = GateMe();
    GateEnter(g);
    DWORD r = R_WaitEx(h, Shorten(ms), a);
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
    HANDLE d;
    if (h && GateTls != TLS_OUT_OF_INDEXES) GateRegister(h, GetThreadId(h));
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
struct XaVoice {
    Proxy px;
    bool in_use, real_dead, recreatable;
    uint32_t created, destroyed;             // XaSerial values
    uint8_t fmt[128]; uint32_t flags; float maxfreq; void* cb;
    uint32_t nsends; struct { uint32_t flags; void* out; } sends[4];
};
static XaVoice  Voices[1024];
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
#define PX_DEF(n) __asm__(".text\n.globl _bscotm_px" #n "\n_bscotm_px" #n ":\n" \
    "    mov 4(%esp), %eax\n    mov 4(%eax), %eax\n    mov %eax, 4(%esp)\n    mov (%eax), %eax\n    jmp *" #n "*4(%eax)\n");
PX_LIST(PX_DECL)
PX_LIST(PX_DEF)
static void* ProxyVt[29] = { PX_LIST(PX_ADDR) };

static bool AnySlot() { for (int i = 0; i < 8; i++) if (SlotSerial[i]) return true; return false; }

static void XaReap() {                       // forget destroyed voices that no saved slot can bring back
    uint32_t keep = 0xFFFFFFFFu;
    for (int i = 0; i < 8; i++) if (SlotSerial[i] && SlotSerial[i] < keep) keep = SlotSerial[i];
    for (auto& v : Voices) if (v.in_use && v.destroyed && v.destroyed < keep) v.in_use = false;
}

static void __stdcall H_DestroyVoice(Proxy* self) {
    EnterCriticalSection(&XaCs);
    XaVoice* v = (XaVoice*)self;
    if (v >= Voices && v < Voices + 1024 && v->in_use && !v->real_dead) {
        void* real = v->px.real;
        v->real_dead = true;
        if (AnySlot()) v->destroyed = ++XaSerial; else v->in_use = false;
        LeaveCriticalSection(&XaCs);
        ((void (__stdcall*)(void*))(*(void***)real)[18])(real);
        return;
    }
    LeaveCriticalSection(&XaCs);
}

static HRESULT __stdcall H_CreateSource(void* self, void** pp, const void* fmt, UINT32 fl, float freq, void* cb,
                                         const void* sends, const void* chain) {
    void* real = nullptr;
    HRESULT hr = R_CreateSrc(self, &real, fmt, fl, freq, cb, sends, chain);
    if (hr != 0 || !real || !pp) { if (pp) *pp = real; return hr; }
    EnterCriticalSection(&XaCs);
    XaVoice* v = nullptr;
    for (auto& c : Voices) if (!c.in_use) { v = &c; break; }
    if (!v) { LeaveCriticalSection(&XaCs); *pp = real; return hr; }       // table full: hand out the real one
    memset(v, 0, sizeof *v);
    v->px.vt = ProxyVt;
    v->px.real = real;
    v->in_use = true;
    v->created = ++XaSerial;
    v->flags = fl; v->maxfreq = freq; v->cb = cb;
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
    XaReap();
    LeaveCriticalSection(&XaCs);
}

static void XaBeforeRestore(int slot) {
    EnterCriticalSection(&XaCs);
    uint32_t T = SlotSerial[slot];
    for (auto& v : Voices) {
        if (!v.in_use) continue;
        if (v.created > T) {                  // born after the save: gone before the memory is put back
            if (!v.real_dead) ((void (__stdcall*)(void*))(*(void***)v.px.real)[18])(v.px.real);
            v.in_use = false;
        } else if (v.destroyed > T) {         // destroyed after the save: bring it back behind the same pointer
            struct { uint32_t n; void* p; } sl = {v.nsends, v.sends};
            void* nv = nullptr;
            if (v.recreatable && R_CreateSrc && R_CreateSrc(XaObject, &nv, v.fmt, v.flags, v.maxfreq, v.cb, v.nsends ? &sl : nullptr, nullptr) == 0 && nv) {
                v.px.real = nv;
                v.real_dead = false;
                v.destroyed = 0;
            }
        }
    }
    LeaveCriticalSection(&XaCs);
}

// ---- savestate commands (handled while the game is held at a marker) ---------------
// After the memory is put back, this fixes up the hook's own state and the game re-enters the
// poll call from its first instruction, so the marker is handled again and the game freezes
// at the same marker it was saved at.
static void SnapAfterCopy(const snap::Slot& sn) {
    Shm* s = S;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    VirtBase = sn.virt;
    RealBase = q.QuadPart;
    CurSpeed = 1000;
    s->speed_milli = 1000;
    s->frame = sn.frame_before;
    s->advance = 0;
    s->hold = 1;
    s->paused = 0;
    NPending = 0;
    XaResume();
    s->snap_result = 1;
    MemoryBarrier();
    s->snap_cmd = 0;
}

static void HandleSnap(Shm* s, uint32_t f, int64_t frozen) {
    uint32_t cmd = s->snap_cmd, slot = s->snap_slot;
    if (slot >= (uint32_t)snap::MAX_SLOTS || !(s->features & FEAT_ARENA_OK)) {
        s->snap_result = 2;
    } else if (cmd == 1) {
        XaPause();
        bool ok = snap::Save((int)slot, frozen, f - 1);
        XaResume();
        if (ok) XaOnSave((int)slot);
        s->snap_frame[slot] = ok ? f : 0;
        s->snap_result = ok ? 1 : 2;
    } else if (cmd == 2 && snap::Slots[slot].valid) {
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
    LeaveCriticalSection(&Cs);
    if (s->mode == M_RECORD) Cur = 0;       // recording starts from a clean frame
    s->paused = 0;
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
            snap::ParkHook = ParkAll;
            snap::UnparkHook = UnparkAll;
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
