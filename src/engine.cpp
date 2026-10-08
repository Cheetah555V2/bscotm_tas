#include "engine.h"
#include "common.h"
#include <tlhelp32.h>
#include <string.h>

#if !defined(BSCOTM_ALLOW_64BIT) && defined(_WIN64)
#error "Build the host as 32-bit: it injects a 32-bit DLL into the 32-bit game."
#endif

static const wchar_t* const SAVE_FILES[] = {
    L"GameData00.bin", L"GameData01.bin", L"GameData02.bin", L"GameData03.bin",
    L"GameData04.bin", L"GameData05.bin", L"GameData06.bin", L"GameData07.bin",
    L"SystemData.bin",
};

static std::wstring DirOf(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? L"." : p.substr(0, i);
}

static void MkDirs(const std::wstring& d) {
    if (d.empty() || GetFileAttributesW(d.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    MkDirs(DirOf(d));
    CreateDirectoryW(d.c_str(), nullptr);
}

// ---- saves -----------------------------------------------------------------
// The game treats save-file existence as authoritative, so we only ever copy
// over existing names; nothing is deleted or renamed.
static int CopySaves(const std::wstring& from, const std::wstring& to) {
    MkDirs(to);
    int n = 0;
    for (const wchar_t* f : SAVE_FILES)
        if (CopyFileW((from + L"\\" + f).c_str(), (to + L"\\" + f).c_str(), FALSE)) n++;
    return n;
}

bool SaveBaseline(const std::wstring& exe, const std::wstring& dir, std::string& err) {
    if (GameRunning()) { err = "Close COTM.exe first: baselines are taken from the files on disk."; return false; }
    if (!CopySaves(DirOf(exe), dir)) { err = "No save files found next to COTM.exe."; return false; }
    return true;
}

// ---- process helpers -------------------------------------------------------
static bool FindGamePid(DWORD* pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof e;
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e))
        if (!_wcsicmp(e.szExeFile, L"COTM.exe")) { *pid = e.th32ProcessID; found = true; }
    CloseHandle(snap);
    return found;
}

bool GameRunning() { DWORD pid; return FindGamePid(&pid); }

void KillGame() {
    DWORD pid;
    while (FindGamePid(&pid)) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        if (!h) return;
        TerminateProcess(h, 0);
        WaitForSingleObject(h, 5000);
        CloseHandle(h);
    }
}

// Returns nullptr on success, else a short description of what failed.
static const char* Inject(HANDLE proc, const std::wstring& dll) {
    SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) return "VirtualAllocEx failed";
    const char* err = nullptr;
    auto ll = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    if (!WriteProcessMemory(proc, mem, dll.c_str(), bytes, nullptr)) {
        err = "WriteProcessMemory failed";
    } else if (HANDLE t = CreateRemoteThread(proc, nullptr, 0, ll, mem, 0, nullptr)) {
        DWORD code = 0;
        if (WaitForSingleObject(t, 20000) != WAIT_OBJECT_0) err = "LoadLibrary timed out";
        else if (!GetExitCodeThread(t, &code) || !code) err = "LoadLibrary returned NULL (DLL missing or failed to load)";
        CloseHandle(t);
    } else {
        err = "CreateRemoteThread failed";
    }
    VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
    return err;
}

// ---- session ----------------------------------------------------------------
bool Session::Alive() const { return proc && WaitForSingleObject(proc, 0) != WAIT_OBJECT_0; }

uint32_t Session::Row() const {
    if (!s || !s->paused) return (uint32_t)-1;
    return s->paused - 1 - pre;
}

bool Session::Step(uint16_t keys) {
    if (!s || !s->paused) return false;
    uint32_t f = s->paused;
    s->keys[f - 1] = keys;
    MemoryBarrier();
    s->advance = 1;
    for (DWORD t0 = GetTickCount(); GetTickCount() - t0 < 3000;) {
        if (s->paused == f + 1) return true;
        if (!Alive()) return false;
        Sleep(1);
    }
    return false;
}

bool Session::BeginSteps(const uint16_t* keys, uint32_t n, uint32_t speed_milli) {
    if (!s || !s->paused || !n || s->paused - 1 + n > MAX_FRAMES) return false;
    uint32_t f = s->paused;
    for (uint32_t i = 0; i < n; i++) s->keys[f - 1 + i] = keys[i];
    step_start = f;
    step_target = f + n;
    step_seen = s->frame;
    step_tick = GetTickCount();
    s->draw_from = step_target;
    s->speed_milli = speed_milli;
    MemoryBarrier();
    s->advance = n;
    return true;
}

uint32_t Session::StepsDone() const {
    uint32_t at = s->paused ? s->paused : s->frame;
    return at > step_start ? at - step_start : 0;
}

int Session::PollSteps() {
    if (s->paused == step_target) { s->speed_milli = 1000; return 1; }
    if (!Alive()) return -1;
    if (s->frame != step_seen) { step_seen = s->frame; step_tick = GetTickCount(); }
    else if (GetTickCount() - step_tick > 5000) return -1;      // no frame for 5 s
    return 0;
}

void Session::AbortSteps() {
    s->advance = 0;
    for (DWORD t0 = GetTickCount(); GetTickCount() - t0 < 3000 && !s->paused && Alive();) Sleep(1);
    s->speed_milli = 1000;
}

bool Session::StartRecording() {
    if (!s || !s->paused) return false;
    rec_start = s->paused - 1;
    s->rec_count = rec_start;
    s->armed = 1;                   // the frame we are about to run is the first one recorded
    s->then_record = 0;
    s->mode = M_RECORD;
    MemoryBarrier();
    s->hold = 0;                    // unfreeze
    return true;
}

uint32_t Session::RecCount() const { return s && s->rec_count > rec_start ? s->rec_count - rec_start : 0; }

bool Session::StopRecording(Frames& out) {
    if (!s) return false;
    s->hold = 1;                    // the hook flushes the last frame, then blocks
    bool frozen = false;
    for (DWORD t0 = GetTickCount(); GetTickCount() - t0 < 3000;) {
        if (s->paused) { frozen = true; break; }
        if (!Alive()) break;
        Sleep(1);
    }
    uint32_t end = s->rec_count;
    out.clear();
    if (end > rec_start) out.assign(s->keys + rec_start, s->keys + end);
    if (frozen) { s->mode = M_PLAY; s->armed = 0; }
    return frozen;
}

void Session::Release() {
    if (s) {
        s->mode = M_IDLE;
        MemoryBarrier();
        s->hold = 0;
    }
    Close();
}

void Session::Close() {
    if (proc) { CloseHandle(thread); CloseHandle(proc); }
    if (s) UnmapViewOfFile(s);
    if (map) CloseHandle(map);
    proc = thread = map = nullptr;
    s = nullptr;
}

// ---- the job ---------------------------------------------------------------
RunResult RunJob(const RunParams& p, const RunCallbacks& cb, Session* keep) {
    RunResult r;
    auto report = [&](Phase ph, uint32_t f) { if (cb.progress) cb.progress(cb.ctx, ph, f); };
    auto stopped = [&] { return cb.stop && *cb.stop; };

    uint64_t total = (uint64_t)p.prelude.size() + p.target;
    if (total + 2 >= MAX_FRAMES) { r.error = "Movie is too long for the shared frame buffer."; return r; }
    if (p.target > p.movie.size()) { r.error = "Target is past the end of the movie."; return r; }

    report(PH_LAUNCH, 0);
    KillGame();
    if (!p.baseline.empty()) {
        if (!p.backup_dir.empty()) CopySaves(DirOf(p.exe), p.backup_dir);
        CopySaves(p.baseline, DirOf(p.exe));
    }

    Session ss;
    ss.pre = (uint32_t)p.prelude.size();
    ss.map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shm), SHM_NAME);
    if (!ss.map) { r.error = "CreateFileMapping failed."; return r; }
    Shm* s = ss.s = (Shm*)MapViewOfFile(ss.map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!s) { ss.Close(); r.error = "MapViewOfFile failed."; return r; }
    memset(s, 0, sizeof(Shm));
    s->magic = SHM_MAGIC;
    s->rng_on = p.seeded ? 1 : 0;       // set before the game starts: it reads the clock during start-up
    s->rng_time = p.seed;
    s->rng_log = p.rng_log ? 1 : 0;
    s->hist_on = p.hist ? 1 : 0;

    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::wstring cwd = DirOf(p.exe);
    std::wstring cmd = L"\"" + p.exe + L"\"";
    if (!CreateProcessW(p.exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                        cwd.c_str(), &si, &pi)) {
        r.error = "Could not start COTM.exe (check the game path).";
        ss.Close();
        return r;
    }
    ss.proc = pi.hProcess;
    ss.thread = pi.hThread;
    const char* ierr = Inject(pi.hProcess, p.dll);
    if (ierr || !(s->status & ST_HOOKED)) {
        r.error = ierr ? std::string("Could not inject bscotm_hook.dll: ") + ierr
                       : (s->status & ST_HOOK_FAIL)
                             ? "Hook DLL loaded, but GetAsyncKeyState was not found in the game's imports."
                             : "Hook DLL loaded but could not open the shared memory.";
        TerminateProcess(pi.hProcess, 0);
        ss.Close();
        return r;
    }
    // Combined schedule: prelude, then movie[0 .. target). It is armed BEFORE the
    // game runs, so frame 1 of the schedule is the game's own first frame marker.
    // (Starting after a wall-clock "ready" wait made the start frame vary from
    // boot to boot, which desynced the prelude.)
    memcpy(s->keys, p.prelude.data(), p.prelude.size() * sizeof(uint16_t));
    memcpy(s->keys + p.prelude.size(), p.movie.data(), p.target * sizeof(uint16_t));
    s->frame = 0;
    s->then_record = p.record ? 1 : 0;
    if (p.hold) {
        s->stop_at = 0xFFFFFFFFu;               // never leaves play mode by itself
        s->hold_at = (uint32_t)total + 1;       // freeze before the first frame past the target
    } else {
        s->stop_at = (uint32_t)total;
    }
    s->draw_from = (uint32_t)total + 1;         // draw again just before the end, so the last picture is real
    s->speed_milli = p.speed_milli;
    s->speed_mask = p.speed_mask;
    s->status &= ~(ST_PLAY_END | ST_RESUMED);
    MemoryBarrier();
    s->mode = M_PLAY;
    ResumeThread(pi.hThread);
    report(PH_BOOT, 0);
    const DWORD boot_t0 = GetTickCount();

    const uint32_t pre = ss.pre;
    bool recording = false, held = false;
    for (;;) {
        Sleep(30);
        if (stopped()) break;
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) { r.error = "The game exited."; break; }
        uint32_t st = s->status;
        if (!s->frame) {
            if (GetTickCount() - boot_t0 > 90000) { r.error = "The game never started polling input."; break; }
            continue;
        }
        if (!recording) {
            uint32_t f = s->frame > pre ? s->frame - pre : 0;
            report(PH_PLAY, f > p.target ? p.target : f);
            if (p.hold && s->paused) { held = true; r.played = p.target; r.ok = true; break; }
            if (st & ST_PLAY_END) { r.played = p.target; r.ok = true; break; }
            if (st & ST_RESUMED) recording = true;
        }
        if (recording) report(PH_RECORD, s->rec_count > pre ? s->rec_count - pre : 0);
    }

    if (held && keep) {         // hand the frozen game over for frame advance
        *keep = ss;
        return r;
    }
    s->hold = 0;
    s->mode = M_IDLE;           // hand the keyboard back; the game keeps running
    MemoryBarrier();
    if (recording) {
        uint32_t end = s->rec_count, begin = (uint32_t)total;
        if (end > begin) r.recorded.assign(s->keys + begin, s->keys + end);
        r.first = p.target;
        r.ok = true;
    } else if (!r.ok && r.error.empty()) {
        r.error = "Stopped.";
        r.played = s->frame > pre ? s->frame - pre : 0;
    }
    ss.Close();         // the hook keeps its own view of the shared block
    return r;
}
