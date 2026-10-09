// bscotm-tas: TAStudio-style frame editor for Bloodstained: Curse of the Moon.
// Plain Win32 + GDI, no external dependencies.
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <tlhelp32.h>
#include <algorithm>
#include <string>
#include <vector>
#include "common.h"
#include "engine.h"
#include "movie.h"

namespace {

enum {
    IDM_NEW = 100, IDM_OPEN, IDM_SAVE, IDM_SAVEAS, IDM_SAVEPRE, IDM_SETGAME, IDM_BASESAVE, IDM_EXIT,
    IDM_UNDO, IDM_REDO, IDM_CUT, IDM_COPY, IDM_PASTE, IDM_PASTEINS, IDM_SELALL,
    IDM_CLEAR, IDM_INSERT, IDM_DELFRAMES, IDM_NOTE_EDIT, IDM_NOTE_DEL, IDM_NOTE_LIST, IDM_RUNTO, IDM_MEMORY, IDM_RNGSEED,
    IDM_REPEAT, IDM_PATTERN, IDM_FIND, IDM_FINDNEXT, IDM_FINDPREV, IDM_NOTENEXT, IDM_NOTEPREV,
    IDC_MEM_HIST, IDC_MEM_GRAPH, IDC_MEM_CSV, IDC_MEM_CLR,
    IDM_HELP_START, IDM_HELP_KEYS, IDM_HELP_TROUBLE, IDM_HELP_ABOUT, IDM_HELP_GUIDE, IDC_HELP_TOPICS,
    IDM_RNGLOG, IDM_VERIFY, IDM_BM_EDIT, IDM_BM_DEL, IDM_BM_LIST, IDM_GOTO, IDM_BM_NEXT, IDM_BM_PREV, IDM_JUMPCUR,
    IDM_SAVESTATES, IDM_SAVESTATE_NOW, IDM_SSINTERVAL,
    IDM_RECORD, IDM_PLAY, IDM_REWIND, IDM_RECFROM, IDM_STOP,
    IDM_SPEED0, IDM_SPEED1, IDM_SPEED2, IDM_SPEED3, IDM_STEP, IDM_RESUME, IDM_RECHERE,
    IDC_BASE = 300, IDC_PRE, IDC_RNG_CHK, IDC_RNG_CLR, IDC_RNG_CSV,
};
const UINT WM_JOB_PROGRESS = WM_APP + 1;   // wParam = Phase, lParam = movie frame
const UINT WM_JOB_DONE     = WM_APP + 2;   // lParam = RunResult*

const COLORREF cBg = RGB(255, 255, 255), cHdr = RGB(236, 236, 240), cLine = RGB(222, 222, 228),
               cLine60 = RGB(140, 140, 155), cGreen = RGB(212, 240, 212), cGreenHead = RGB(140, 205, 140),
               cSel = RGB(204, 224, 255), cCursor = RGB(40, 90, 180), cPressed = RGB(52, 120, 200),
               cPhantom = RGB(170, 170, 175), cNote = RGB(255, 249, 212), cNoteText = RGB(90, 60, 0),
               cFlag = RGB(235, 170, 20), cBm = RGB(238, 226, 250), cBmFlag = RGB(140, 70, 200), cBmText = RGB(80, 30, 130);

typedef std::vector<std::string> Notes;   // UTF-8, parallel to the frames

// One undoable edit = a list of "replace range" operations (frames and their notes).
struct Splice { size_t pos; Frames oldv, newv; Notes oldn, newn; };
struct Step   { std::vector<Splice> parts; int cur_before = 0, cur_after = 0; };

struct App {
    HWND wnd = nullptr, grid = nullptr, status = nullptr, cbBase = nullptr, cbPre = nullptr;
    HWND lblBase = nullptr, lblPre = nullptr, btn[8] = {};
    HFONT font = nullptr, fontB = nullptr;
    HWND notesWnd = nullptr, notesList = nullptr;   // the Notes / Bookmarks list window (when open)
    bool listBm = false;                            // that window shows only the bookmarks
    HWND memWnd = nullptr, memList = nullptr;       // the Game memory window (when open)
    HWND rngWnd = nullptr, rngList = nullptr;       // the RNG log window (when open)
    HWND graphWnd = nullptr, graphList = nullptr;   // the value graph window (when open)
    bool histOn = false;                            // record the player's values every frame (Memory window's checkbox)
    bool rngLogOn = false;                          // log the game's random draws (set by the window's checkbox)
    int logPre = 0;                                 // prelude length of the last launch (marker number = movie frame + this)
    int dpi = 96;

    Movie movie;
    std::wstring path;
    bool dirty = false;
    int cursor = 0, anchor = 0;   // selection = [anchor, cursor]
    int reached = 0;              // "greenzone": frames the live game has been advanced through
    int top = 0;                  // first visible row
    std::vector<Step> undo, redo;
    Frames clip;
    Notes clipNotes;

    Step drag;                   // paint-drag in progress
    bool dragging = false, selecting = false, dragVal = false;
    int dragCol = 0, dragRow = 0;

    int speed = 3;                // index into SPEEDS: fast-forward while replaying to the cursor
    HMENU speedMenu = nullptr;
    Session sess;                 // the frozen game frame advance steps (when Active)
    Session jobSess;              // handed over by a finished hold job
    bool liveRec = false;         // recording live from the frozen game (busy is set too)
    int liveRecRow = 0;           // movie row the live recording started at
    bool pendingStep = false;     // step once the resync job finishes
    bool pendingRun = false;      // run to the cursor once the resync job finishes
    bool useStates = true;        // Run > Use savestates (on by default): launch the game with the private heap so states can be saved
    int  ssEvery = 2000;          // automatic state every this many frames while the game is advanced (0 = never)
    bool ssManual[8] = {};        // slots saved by hand (never evicted automatically)
    int  chunkFrom = 0;           // first row of the batch being run (Run to cursor runs in pieces so states can be saved between them)
    bool stepping = false;        // a batch of frames is running (busy is set too)
    int runFrom = 0, runTarget = 0;   // rows the batch started at / will stop before
    bool busy = false, jobRecord = false, jobNew = false, jobHold = false;
    uint32_t jobTarget = 0;
    volatile LONG stop = 0;
    HANDLE thread = nullptr;

    std::wstring exe, root, toolDir, ini;
} A;

int S(int v) { return MulDiv(v, A.dpi, 96); }
int RowH()   { return S(18); }
int HdrH()   { return S(24); }
int FrameW() { return S(64); }
int KeyW()   { return S(38); }
int Size()   { return (int)A.movie.frames.size(); }

std::wstring W(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
std::string U8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// ---- status / title --------------------------------------------------------
void SetMsg(const std::wstring& m) { SendMessageW(A.status, SB_SETTEXTW, 0, (LPARAM)m.c_str()); }

int NumStates();
void UpdateStatus() {
    int n = Size();
    wchar_t b[192], sd[64] = L"";
    if (A.movie.has_seed) swprintf(sd, 40, L"   seed %u", (unsigned)A.movie.seed);
    if (int ns = NumStates()) swprintf(sd + wcslen(sd), 64 - wcslen(sd), L"   states %d", ns);
    double t = (A.cursor + 1) / 60.0, tt = n / 60.0;
    swprintf(b, 192, L"Frame %d / %d   (%d:%05.2f / %d:%05.2f)   game at %d%ls", n ? A.cursor + 1 : 0, n,
             (int)t / 60, t - 60 * ((int)t / 60), (int)tt / 60, tt - 60 * ((int)tt / 60), A.reached, sd);
    SendMessageW(A.status, SB_SETTEXTW, 1, (LPARAM)b);
}

void UpdateTitle() {
    std::wstring name = A.path.empty() ? L"untitled" : A.path.substr(A.path.find_last_of(L"\\/") + 1);
    SetWindowTextW(A.wnd, (L"bscotm-tas - " + name + (A.dirty ? L" *" : L"")).c_str());
}

// ---- grid geometry / scrolling -------------------------------------------
int VisibleRows() {
    RECT rc;
    GetClientRect(A.grid, &rc);
    return std::max(1, (int)((rc.bottom - HdrH()) / RowH()));
}

void UpdateScroll() {
    int vis = VisibleRows();
    A.top = std::max(0, std::min(A.top, Size()));
    SCROLLINFO si{sizeof si, SIF_RANGE | SIF_PAGE | SIF_POS, 0, Size() + vis - 1, (UINT)vis, A.top, 0};
    SetScrollInfo(A.grid, SB_VERT, &si, TRUE);
    InvalidateRect(A.grid, nullptr, FALSE);
}

void SetCursorRow(int row, bool extend) {
    row = std::max(0, std::min(row, Size()));
    A.cursor = row;
    if (!extend) A.anchor = row;
    int vis = VisibleRows();
    if (row < A.top) A.top = row;
    else if (row >= A.top + vis - 1) A.top = row - vis + 2;
    UpdateScroll();
    UpdateStatus();
}

// ---- editing (every change goes through DoSplice so it is undoable) -------
void Touch(size_t pos) {
    A.dirty = true;
    if ((int)pos < A.reached) A.reached = (int)pos;
}

void RefreshNotesList();
void JumpTo(int row);

const std::string& NoteAt(int row) {
    static const std::string none;
    return row >= 0 && (size_t)row < A.movie.notes.size() ? A.movie.notes[row] : none;
}

// A bookmark is a frame note with a flag: the note string starts with BM_FLAG (never typed by the
// user), so bookmarks move with their frames and are covered by undo like notes are. A bookmark can
// have an empty name, a plain note cannot be empty.
const char BM_FLAG = kBookmarkFlag;
bool IsBm(const std::string& s) { return !s.empty() && s[0] == BM_FLAG; }
std::string NoteBody(const std::string& s) { return IsBm(s) ? s.substr(1) : s; }
bool RowIsBm(int row) { return IsBm(NoteAt(row)); }

void Replace(size_t pos, size_t oldlen, const Frames& nv, const Notes& nn) {
    Frames& f = A.movie.frames;
    Notes& n = A.movie.notes;
    n.resize(f.size());
    f.erase(f.begin() + pos, f.begin() + pos + oldlen);
    f.insert(f.begin() + pos, nv.begin(), nv.end());
    n.erase(n.begin() + pos, n.begin() + pos + oldlen);
    n.insert(n.begin() + pos, nn.begin(), nn.end());
}

// Replace frames [pos, pos+oldlen) with nv. Without `nn` the notes of the replaced
// range are kept in place (padded with empty ones / dropped if the range shrinks), so
// editing inputs never disturbs notes while inserting and deleting frames moves them.
void DoSplice(Step& st, size_t pos, size_t oldlen, Frames nv, const Notes* nn = nullptr) {
    Frames& f = A.movie.frames;
    A.movie.notes.resize(f.size());
    pos = std::min(pos, f.size());
    oldlen = std::min(oldlen, f.size() - pos);
    const Notes& n = A.movie.notes;
    Splice s{pos, Frames(f.begin() + pos, f.begin() + pos + oldlen), std::move(nv),
             Notes(n.begin() + pos, n.begin() + pos + oldlen), Notes()};
    s.newn = nn ? *nn : s.oldn;
    s.newn.resize(s.newv.size());
    Replace(pos, oldlen, s.newv, s.newn);
    if (s.oldv != s.newv) Touch(pos);     // a note-only edit leaves the greenzone alone
    else A.dirty = true;
    st.parts.push_back(std::move(s));
}

void EnsureRows(Step& st, size_t n) {
    if (A.movie.frames.size() < n) DoSplice(st, A.movie.frames.size(), 0, Frames(n - A.movie.frames.size(), 0));
}

void Commit(Step& st) {
    if (st.parts.empty()) return;
    st.cur_after = A.cursor;
    A.undo.push_back(std::move(st));
    if (A.undo.size() > 500) A.undo.erase(A.undo.begin());
    A.redo.clear();
    st = Step();
    UpdateTitle();
    RefreshNotesList();
}

void RunStep(const Step& st, bool undo) {
    size_t lo = (size_t)-1;
    A.dirty = true;
    if (undo) {
        for (auto it = st.parts.rbegin(); it != st.parts.rend(); ++it) {
            Replace(it->pos, it->newv.size(), it->oldv, it->oldn);
            if (it->oldv != it->newv) lo = std::min(lo, it->pos);
        }
    } else {
        for (auto& p : st.parts) {
            Replace(p.pos, p.oldv.size(), p.newv, p.newn);
            if (p.oldv != p.newv) lo = std::min(lo, p.pos);
        }
    }
    if (lo != (size_t)-1) Touch(lo);
    UpdateTitle();
    RefreshNotesList();
    A.cursor = A.anchor = undo ? st.cur_before : st.cur_after;
    SetCursorRow(A.cursor, false);
}

void Undo() {
    if (A.busy || A.undo.empty()) return;
    Step st = std::move(A.undo.back());
    A.undo.pop_back();
    RunStep(st, true);
    A.redo.push_back(std::move(st));
}

void Redo() {
    if (A.busy || A.redo.empty()) return;
    Step st = std::move(A.redo.back());
    A.redo.pop_back();
    RunStep(st, false);
    A.undo.push_back(std::move(st));
}

void SetCell(Step& st, int row, int bit, bool on) {
    EnsureRows(st, row + 1);
    uint16_t m = A.movie.frames[row], n = on ? (uint16_t)(m | (1u << bit)) : (uint16_t)(m & ~(1u << bit));
    if (n != m) DoSplice(st, row, 1, Frames{n});
}

int SelLo() { return std::min(A.anchor, A.cursor); }
int SelHi() { return std::max(A.anchor, A.cursor); }

void EditClear() {
    if (A.busy || !Size()) return;
    Step st; st.cur_before = A.cursor;
    int lo = SelLo(), hi = std::min(SelHi(), Size() - 1);
    if (hi >= lo) DoSplice(st, lo, hi - lo + 1, Frames(hi - lo + 1, 0));
    Commit(st); InvalidateRect(A.grid, nullptr, FALSE);
}

void EditInsert() {
    if (A.busy) return;
    Step st; st.cur_before = A.cursor;
    int n = SelHi() - SelLo() + 1, lo = std::min(SelLo(), Size());
    DoSplice(st, lo, 0, Frames(n, 0));
    Commit(st); UpdateScroll(); UpdateStatus();
}

void EditDelete() {
    if (A.busy || !Size()) return;
    Step st; st.cur_before = A.cursor;
    int lo = SelLo(), hi = std::min(SelHi(), Size() - 1);
    if (hi >= lo) DoSplice(st, lo, hi - lo + 1, Frames());
    A.cursor = A.anchor = lo;
    Commit(st);
    SetCursorRow(lo, false);
}

void EditCopy() {
    if (!Size()) return;
    int lo = SelLo(), hi = std::min(SelHi(), Size() - 1);
    if (hi < lo) return;
    A.clip.assign(A.movie.frames.begin() + lo, A.movie.frames.begin() + hi + 1);
    A.clipNotes.clear();
    for (int r = lo; r <= hi; r++) A.clipNotes.push_back(NoteAt(r));
}

void EditCut() { EditCopy(); EditDelete(); }

void EditPaste(bool insert) {
    if (A.busy || A.clip.empty()) return;
    Step st; st.cur_before = A.cursor;
    size_t at = (size_t)std::min(A.cursor, Size());
    if (insert) {
        DoSplice(st, at, 0, A.clip, &A.clipNotes);
    } else {            // overwrite: a pasted frame without a note keeps the note already there
        EnsureRows(st, at + A.clip.size());
        Notes nn = A.clipNotes;
        for (size_t i = 0; i < nn.size(); i++) if (nn[i].empty()) nn[i] = NoteAt((int)(at + i));
        DoSplice(st, at, A.clip.size(), A.clip, &nn);
    }
    Commit(st); UpdateScroll(); UpdateStatus();
}

// ---- frame notes ------------------------------------------------------------
// One note per frame. Setting an empty text removes it.
void SetNote(int row, const std::string& text) {
    if (A.busy || row < 0) return;
    if (row < Size() && NoteAt(row) == text) return;
    if (row >= Size() && text.empty()) return;
    Step st; st.cur_before = A.cursor;
    EnsureRows(st, row + 1);
    Notes nn{text};
    DoSplice(st, row, 1, Frames{A.movie.frames[row]}, &nn);
    Commit(st);
    InvalidateRect(A.grid, nullptr, FALSE);
}

bool SelectionHasNote() {
    for (int r = SelLo(); r <= std::min(SelHi(), Size() - 1); r++)
        if (!NoteBody(NoteAt(r)).empty()) return true;
    return false;
}

void RemoveNotesInSelection() {
    if (A.busy || !Size() || !SelectionHasNote()) return;
    int lo = SelLo(), hi = std::min(SelHi(), Size() - 1);
    Step st; st.cur_before = A.cursor;
    Notes none(hi - lo + 1);
    for (int r = lo; r <= hi; r++) if (RowIsBm(r)) none[r - lo] = std::string(1, BM_FLAG);   // bookmarks stay
    DoSplice(st, lo, hi - lo + 1, Frames(A.movie.frames.begin() + lo, A.movie.frames.begin() + hi + 1), &none);
    Commit(st);
    InvalidateRect(A.grid, nullptr, FALSE);
}

// Small modal "enter text" box. Returns false on Cancel.
struct NoteDlg { HWND wnd = nullptr, edit = nullptr; std::wstring text; bool ok = false, done = false; };

LRESULT CALLBACK NoteDlgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    NoteDlg* d = (NoteDlg*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
        case WM_CREATE: SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams); return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == IDOK && d) {
                int n = GetWindowTextLengthW(d->edit);
                d->text.assign(n + 1, 0);
                GetWindowTextW(d->edit, &d->text[0], n + 1);
                d->text.resize(n);
                d->ok = d->done = true;
            } else if (LOWORD(wp) == IDCANCEL && d) {
                d->done = true;
            }
            return 0;
        case WM_CLOSE: if (d) d->done = true; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool AskText(const std::wstring& title, const std::wstring& prompt, std::wstring& io) {
    NoteDlg d;
    d.text = io;
    RECT pr;
    GetWindowRect(A.wnd, &pr);
    int w = S(480), h = S(140);
    d.wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"BscotmNote", title.c_str(), WS_POPUP | WS_CAPTION | WS_SYSMENU,
                            (pr.left + pr.right - w) / 2, (pr.top + pr.bottom - h) / 2, w, h, A.wnd, nullptr, nullptr, &d);
    if (!d.wnd) return false;
    HWND lbl = CreateWindowExW(0, L"STATIC", prompt.c_str(), WS_CHILD | WS_VISIBLE, S(12), S(10), w - S(40), S(18), d.wnd, nullptr, nullptr, nullptr);
    d.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", io.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                             S(12), S(32), w - S(40), S(24), d.wnd, nullptr, nullptr, nullptr);
    HWND bo = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              w - S(12) - S(180), S(70), S(80), S(26), d.wnd, (HMENU)(INT_PTR)IDOK, nullptr, nullptr);
    HWND bc = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                              w - S(12) - S(92), S(70), S(80), S(26), d.wnd, (HMENU)(INT_PTR)IDCANCEL, nullptr, nullptr);
    for (HWND c : {lbl, d.edit, bo, bc}) SendMessageW(c, WM_SETFONT, (WPARAM)A.font, TRUE);
    SendMessageW(d.edit, EM_SETLIMITTEXT, 200, 0);
    EnableWindow(A.wnd, FALSE);
    ShowWindow(d.wnd, SW_SHOW);
    SetFocus(d.edit);
    SendMessageW(d.edit, EM_SETSEL, 0, -1);
    MSG m;
    while (!d.done && GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (m.message == WM_KEYDOWN && (m.wParam == VK_RETURN || m.wParam == VK_ESCAPE) &&
            (m.hwnd == d.edit || m.hwnd == d.wnd)) {
            SendMessageW(d.wnd, WM_COMMAND, m.wParam == VK_RETURN ? IDOK : IDCANCEL, 0);
            continue;
        }
        if (!IsDialogMessageW(d.wnd, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    EnableWindow(A.wnd, TRUE);
    DestroyWindow(d.wnd);
    SetForegroundWindow(A.wnd);
    if (d.ok) io = d.text;
    return d.ok;
}

void EditNote() {
    if (A.busy) return;
    int row = A.cursor;
    bool bm = RowIsBm(row);
    std::wstring t = W(NoteBody(NoteAt(row)));
    if (!AskText(L"Frame note", L"Note for frame " + std::to_wstring(row + 1) + L" (leave empty to remove):", t)) return;
    for (wchar_t& c : t) if (c == L'\r' || c == L'\n' || c == L'\t' || c == (wchar_t)BM_FLAG) c = L' ';
    SetNote(row, (bm ? std::string(1, BM_FLAG) : std::string()) + U8(t));
    SetFocus(A.grid);
}

// Repeat the selected rows N more times right after the selection (inserted frames).
void EditRepeat() {
    if (A.busy || !Size()) return;
    int lo = SelLo(), hi = std::min(SelHi(), Size() - 1);
    if (hi < lo) return;
    std::wstring t = L"1";
    if (!AskText(L"Repeat selection", L"Copy the " + std::to_wstring(hi - lo + 1) + L" selected frame(s) this many more times (1 - 1000):", t)) return;
    wchar_t* end = nullptr;
    long n = wcstol(t.c_str(), &end, 10);
    if (t.empty() || *end || n < 1 || n > 1000 || (size_t)(hi - lo + 1) * (size_t)n + (size_t)Size() >= (size_t)MAX_FRAMES - 4) {
        MessageBoxW(A.wnd, L"Enter a whole number from 1 to 1000 (the movie must stay under the frame limit).", L"Repeat selection", MB_ICONERROR);
        return;
    }
    Frames block(A.movie.frames.begin() + lo, A.movie.frames.begin() + hi + 1), nv;
    for (long i = 0; i < n; i++) nv.insert(nv.end(), block.begin(), block.end());
    Step st; st.cur_before = A.cursor;
    DoSplice(st, hi + 1, 0, nv);
    A.anchor = lo;
    A.cursor = hi + (int)nv.size();
    Commit(st);
    UpdateScroll(); UpdateStatus();
    SetMsg(L"Repeated the selection " + std::to_wstring(n) + L" more time(s).");
}

// Fill the selection with a repeating press of one key: "KEY period on [offset]", e.g. "Jmp 12 2" presses
// the jump key for 2 frames out of every 12; the key is released on all the other selected frames.
static int KeyByName(const std::wstring& w) {
    std::string a = U8(w);
    for (char& c : a) c = (char)tolower((unsigned char)c);
    for (int i = 0; i < NUM_KEYS; i++) {
        std::string n = KEYS[i].name, l = KEYS[i].label;
        for (char& c : n) c = (char)tolower((unsigned char)c);
        for (char& c : l) c = (char)tolower((unsigned char)c);
        if (a == n || a == l) return i;
    }
    return -1;
}

void EditPattern() {
    if (A.busy) return;
    int lo = SelLo(), hi = SelHi();
    if (hi - lo + 1 < 2) { SetMsg(L"Select the frames to fill first (click a frame number, Shift+click another)."); return; }
    std::wstring t = L"Jmp 12 2";
    std::wstring keys;
    for (int i = 0; i < NUM_KEYS; i++) { if (i) keys += L" "; keys += W(KEYS[i].label); }
    if (!AskText(L"Fill pattern", L"KEY period on [offset] for the " + std::to_wstring(hi - lo + 1) + L" selected frames. Keys: " + keys, t)) return;
    std::vector<std::wstring> tok;
    {
        size_t i = 0;
        while (i < t.size()) {
            while (i < t.size() && t[i] == L' ') i++;
            size_t j = i;
            while (j < t.size() && t[j] != L' ') j++;
            if (j > i) tok.push_back(t.substr(i, j - i));
            i = j;
        }
    }
    int key = tok.size() >= 3 ? KeyByName(tok[0]) : -1;
    wchar_t* e1 = nullptr; wchar_t* e2 = nullptr; wchar_t* e3 = nullptr;
    long period = tok.size() >= 3 ? wcstol(tok[1].c_str(), &e1, 10) : 0;
    long on = tok.size() >= 3 ? wcstol(tok[2].c_str(), &e2, 10) : -1;
    long offset = tok.size() >= 4 ? wcstol(tok[3].c_str(), &e3, 10) : 0;
    if (key < 0 || tok.size() > 4 || period < 1 || on < 0 || on > period || offset < 0 || *e1 || *e2 || (e3 && *e3)) {
        MessageBoxW(A.wnd, L"Format: KEY period on [offset]\nfor example  Jmp 12 2  (jump pressed for 2 frames out of every 12),\nor  R 1 1  (hold right on every frame).\n\nperiod is at least 1, on is 0 to period, offset is how many frames to skip first.",
                    L"Fill pattern", MB_ICONERROR);
        return;
    }
    Step st; st.cur_before = A.cursor;
    EnsureRows(st, (size_t)hi + 1);
    Frames nv(A.movie.frames.begin() + lo, A.movie.frames.begin() + hi + 1);
    for (int k = 0; k <= hi - lo; k++) {
        bool down = k >= offset && ((k - offset) % period) < on;
        if (down) nv[k] |= (uint16_t)(1u << key); else nv[k] &= (uint16_t)~(1u << key);
    }
    DoSplice(st, lo, hi - lo + 1, nv);
    Commit(st);
    InvalidateRect(A.grid, nullptr, FALSE);
    UpdateScroll(); UpdateStatus();
    SetMsg(L"Filled " + std::to_wstring(hi - lo + 1) + L" frames with the pattern.");
}
// ---- find in the grid -----------------------------------------------------------------------------------
// Find: move the cursor to the next / previous frame where a key is pressed (goes from up to down), released
// (down to up) or any key changes. Notes: jump to the next / previous frame that has a note or bookmark.
static int FindKey = -1;           // -1 = any key
static int FindMode = 0;           // 0 = pressed, 1 = released, 2 = any change

static bool FindMatch(int row) {
    uint16_t cur = A.movie.frames[row], prev = row > 0 ? A.movie.frames[row - 1] : 0;
    if (FindMode == 2 || FindKey < 0) return FindKey < 0 ? cur != prev : ((cur ^ prev) >> FindKey & 1) != 0;
    bool now = cur >> FindKey & 1, was = prev >> FindKey & 1;
    return FindMode == 0 ? (now && !was) : (!now && was);
}

static std::wstring FindDescription() {
    std::wstring k = FindKey < 0 ? L"any key" : W(KEYS[FindKey].label);
    return FindMode == 0 ? k + L" pressed" : FindMode == 1 ? k + L" released" : k + L" changes";
}

void FindStep(bool forward) {
    if (A.busy || !Size()) return;
    int n = Size();
    for (int r = A.cursor + (forward ? 1 : -1); r >= 0 && r < n; r += forward ? 1 : -1)
        if (FindMatch(r)) { SetCursorRow(r, false); SetMsg(L"Found: " + FindDescription() + L" at frame " + std::to_wstring(r + 1) + L". F3 / Shift+F3 for the next / previous."); return; }
    SetMsg(L"No more frames where " + FindDescription() + (forward ? L" after the cursor." : L" before the cursor."));
}

void FindDialog() {
    if (A.busy || !Size()) return;
    std::wstring t = FindKey < 0 ? L"*" : W(KEYS[FindKey].label);
    if (FindKey >= 0 && FindMode == 1) t += L" released";
    std::wstring keys;
    for (int i = 0; i < NUM_KEYS; i++) { if (i) keys += L" "; keys += W(KEYS[i].label); }
    if (!AskText(L"Find", L"KEY [pressed|released], or * for any change. Keys: " + keys, t)) return;
    std::vector<std::wstring> tok;
    size_t i = 0;
    while (i < t.size()) {
        while (i < t.size() && t[i] == L' ') i++;
        size_t j = i;
        while (j < t.size() && t[j] != L' ') j++;
        if (j > i) tok.push_back(t.substr(i, j - i));
        i = j;
    }
    int key = -2, mode = 0;
    if (!tok.empty() && tok.size() <= 2) {
        key = tok[0] == L"*" ? -1 : KeyByName(tok[0]);
        if (tok.size() == 2) {
            std::string m = U8(tok[1]);
            for (char& c : m) c = (char)tolower((unsigned char)c);
            if (m == "pressed" || m == "press" || m == "down") mode = 0;
            else if (m == "released" || m == "release" || m == "up") mode = 1;
            else key = -2;
        }
    }
    if (key == -2) {
        MessageBoxW(A.wnd, L"Format: KEY [pressed|released]\nfor example  Jmp  or  Atk released  or  *  (any key changes).", L"Find", MB_ICONERROR);
        return;
    }
    FindKey = key; FindMode = key < 0 ? 2 : mode;
    FindStep(true);
}

void NoteStep(bool forward) {
    if (A.busy || !Size()) return;
    for (int r = A.cursor + (forward ? 1 : -1); r >= 0 && r < Size(); r += forward ? 1 : -1)
        if (!NoteAt(r).empty()) { SetCursorRow(r, false); SetMsg(L"Note at frame " + std::to_wstring(r + 1) + L"."); return; }
    SetMsg(forward ? L"No note after the cursor." : L"No note before the cursor.");
}
// Bookmarks: named frames to jump to (see JumpTo). Stored as flagged notes.
void EditBookmark() {
    if (A.busy || !Size()) return;
    int row = A.cursor;
    std::wstring t = W(NoteBody(NoteAt(row)));
    if (!AskText(L"Bookmark", L"Name of the bookmark at frame " + std::to_wstring(row + 1) + L":", t)) return;
    for (wchar_t& c : t) if (c == L'\r' || c == L'\n' || c == L'\t' || c == (wchar_t)BM_FLAG) c = L' ';
    SetNote(row, std::string(1, BM_FLAG) + U8(t));
    SetFocus(A.grid);
}

void RemoveBookmark(int row) {
    if (A.busy || !RowIsBm(row)) return;
    SetNote(row, NoteBody(NoteAt(row)));
}

void NextBookmark(bool forward) {
    int best = -1;
    if (forward) { for (int r = A.cursor + 1; r < Size(); r++) if (RowIsBm(r)) { best = r; break; } }
    else         { for (int r = A.cursor - 1; r >= 0; r--)     if (RowIsBm(r)) { best = r; break; } }
    if (best < 0) { SetMsg(forward ? L"No bookmark after the cursor." : L"No bookmark before the cursor."); return; }
    JumpTo(best);
}

void GoToFrame() {
    if (A.busy || !Size()) return;
    std::wstring t = std::to_wstring(A.cursor + 1);
    if (!AskText(L"Jump to frame", L"Frame number (1 - " + std::to_wstring(Size()) + L"):", t)) return;
    wchar_t* end = nullptr;
    long n = wcstol(t.c_str(), &end, 10);
    if (t.empty() || *end || n < 1) { MessageBoxW(A.wnd, L"Enter a frame number.", L"Jump to frame", MB_ICONERROR); return; }
    JumpTo((int)std::min<long>(n, Size()) - 1);
}

// The RNG seed is the Unix time (seconds) the game sees at launch; it seeds the game's generator.
// Changing it makes the live game stale, so the next frame advance replays from the start.
void EditSeed() {
    if (A.busy) return;
    std::wstring t = A.movie.has_seed ? std::to_wstring(A.movie.seed) : L"";
    if (!AskText(L"RNG seed", L"Seed (a number 0 - 4294967295; leave empty for the real clock = random every launch):", t)) return;
    while (!t.empty() && t.back() == L' ') t.pop_back();
    size_t b = t.find_first_not_of(L' ');
    t = b == std::wstring::npos ? L"" : t.substr(b);
    bool has = !t.empty();
    uint64_t v = 0;
    if (has) {
        if (t.size() > 10 || t.find_first_not_of(L"0123456789") != std::wstring::npos) has = false;
        else v = _wcstoui64(t.c_str(), nullptr, 10);
        if (!has || v > 4294967295ull) {
            MessageBoxW(A.wnd, L"The seed must be a whole number from 0 to 4294967295.", L"RNG seed", MB_ICONERROR);
            return;
        }
    }
    if (has == A.movie.has_seed && (!has || (uint32_t)v == A.movie.seed)) return;
    A.movie.has_seed = has;
    A.movie.seed = (uint32_t)v;
    A.dirty = true;
    A.reached = 0;
    InvalidateRect(A.grid, nullptr, FALSE);
    UpdateTitle(); UpdateStatus();
    SetMsg(has ? L"RNG seed " + std::to_wstring(A.movie.seed) + L". Rewind (F6) to restart the game with it."
               : L"RNG seed removed: the game uses the real clock. Rewind (F6) to restart it.");
    SetFocus(A.grid);
}

void SelectAll() {
    A.anchor = 0; A.cursor = std::max(0, Size() - 1);
    InvalidateRect(A.grid, nullptr, FALSE);
    UpdateStatus();
}

// ---- grid window ------------------------------------------------------------
void Fill(HDC dc, int l, int t, int r, int b, COLORREF c) {
    RECT rc{l, t, r, b};
    SetBkColor(dc, c);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &rc, nullptr, 0, nullptr);
}

void Text(HDC dc, const wchar_t* s, RECT rc, UINT fmt, COLORREF c) {
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &rc, fmt | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}

void PaintGrid(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    HDC m = CreateCompatibleDC(dc);
    HBITMAP bm = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ oldbm = SelectObject(m, bm);
    SelectObject(m, A.font);
    SetBkMode(m, TRANSPARENT);

    const int rh = RowH(), hh = HdrH(), fw = FrameW(), kw = KeyW(), gw = fw + NUM_KEYS * kw;
    Fill(m, 0, 0, rc.right, rc.bottom, cBg);

    // header
    Fill(m, 0, 0, rc.right, hh, cHdr);
    SelectObject(m, A.fontB);
    Text(m, L"Frame", RECT{0, 0, fw - S(6), hh}, DT_RIGHT, RGB(0, 0, 0));
    Text(m, L"Note", RECT{gw + S(6), 0, rc.right, hh}, DT_LEFT, RGB(0, 0, 0));
    for (int k = 0; k < NUM_KEYS; k++) {
        wchar_t lb[8];
        swprintf(lb, 8, L"%hs", KEYS[k].label);
        Text(m, lb, RECT{fw + k * kw, 0, fw + (k + 1) * kw, hh}, DT_CENTER, RGB(0, 0, 0));
    }
    Fill(m, 0, hh - 1, rc.right, hh, cLine60);
    SelectObject(m, A.font);

    const int lo = SelLo(), hi = SelHi(), n = Size();
    for (int i = 0;; i++) {
        int r = A.top + i, y = hh + i * rh;
        if (y >= rc.bottom) break;
        bool real = r < n, sel = r >= lo && r <= hi;
        uint16_t mask = real ? A.movie.frames[r] : 0;

        Fill(m, 0, y, fw, y + rh, r < A.reached ? (r == A.reached - 1 ? cGreenHead : cGreen) : cHdr);
        wchar_t num[16];
        swprintf(num, 16, L"%d", r + 1);
        Text(m, num, RECT{0, y, fw - S(6), y + rh}, DT_RIGHT, real ? RGB(0, 0, 0) : cPhantom);
        Fill(m, fw, y, gw, y + rh, sel ? cSel : cBg);
        const std::string& note = NoteAt(r);
        bool bm = IsBm(note);
        Fill(m, gw, y, rc.right, y + rh, sel ? cSel : (note.empty() ? cBg : bm ? cBm : cNote));
        if (!note.empty()) {
            Fill(m, S(2), y + S(4), S(8), y + rh - S(4), bm ? cBmFlag : cFlag);   // flag next to the frame number
            std::wstring wn = (bm ? L"\x2605 " : L"") + W(NoteBody(note));       // bookmarks show a star
            Text(m, wn.c_str(), RECT{gw + S(6), y, rc.right - S(4), y + rh}, DT_LEFT | DT_END_ELLIPSIS, bm ? cBmText : cNoteText);
        }

        for (int k = 0; k < NUM_KEYS; k++) {
            if (!(mask >> k & 1)) continue;
            int x = fw + k * kw;
            Fill(m, x + 1, y + 1, x + kw, y + rh, cPressed);
            wchar_t lb[8];
            swprintf(lb, 8, L"%hs", KEYS[k].label);
            Text(m, lb, RECT{x, y, x + kw, y + rh}, DT_CENTER, RGB(255, 255, 255));
        }
        for (int k = 0; k <= NUM_KEYS; k++) Fill(m, fw + k * kw, y, fw + k * kw + 1, y + rh, cLine);
        Fill(m, gw, y, gw + 1, y + rh, cLine60);
        Fill(m, 0, y + rh - 1, rc.right, y + rh, (r + 1) % 60 == 0 ? cLine60 : cLine);
        if (r == A.cursor) {
            Fill(m, fw, y, rc.right, y + 2, cCursor);
            Fill(m, fw, y + rh - 2, rc.right, y + rh, cCursor);
        }
    }
    Fill(m, fw - 1, hh, fw, rc.bottom, cLine60);
    Fill(m, gw, hh, gw + 1, rc.bottom, cLine60);

    BitBlt(dc, 0, 0, rc.right, rc.bottom, m, 0, 0, SRCCOPY);
    SelectObject(m, oldbm);
    DeleteObject(bm);
    DeleteDC(m);
    EndPaint(h, &ps);
}

int RowAt(int y) { return A.top + (y < HdrH() ? -1 : (y - HdrH()) / RowH()); }
int ColAt(int x) { return x < FrameW() ? -1 : (x - FrameW()) / KeyW(); }   // >= NUM_KEYS: outside

void PaintTo(int row) {     // paint dragCol from the last row to `row`
    int a = A.dragRow, b = row, step = a <= b ? 1 : -1;
    for (int r = a;; r += step) {
        if (r >= 0) SetCell(A.drag, r, A.dragCol, A.dragVal);
        if (r == b) break;
    }
    A.dragRow = row;
}

LRESULT CALLBACK GridProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: PaintGrid(h); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_SIZE: UpdateScroll(); return 0;
        case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_LBUTTONDOWN: {
            SetFocus(h);
            int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
            if (A.busy || y < HdrH()) return 0;
            int row = RowAt(y), col = ColAt(x);
            SetCapture(h);
            if (col < 0 || col >= NUM_KEYS) {       // frame number or note cell: select rows
                A.selecting = true;
                SetCursorRow(row, GetKeyState(VK_SHIFT) < 0);
                return 0;
            }
            A.drag = Step();
            A.drag.cur_before = A.cursor;
            A.dragging = true;
            A.dragCol = col;
            A.dragRow = row;
            A.dragVal = !(row < Size() && (A.movie.frames[row] >> col & 1));
            SetCell(A.drag, row, col, A.dragVal);
            SetCursorRow(row, false);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!(wp & MK_LBUTTON)) return 0;
            int row = std::max(0, RowAt((short)HIWORD(lp)));
            if (A.selecting) SetCursorRow(row, true);
            else if (A.dragging && row != A.dragRow) { PaintTo(row); SetCursorRow(row, false); }
            return 0;
        }
        case WM_LBUTTONUP:
            if (A.dragging) Commit(A.drag);
            A.dragging = A.selecting = false;
            ReleaseCapture();
            return 0;
        case WM_RBUTTONUP: {
            int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
            if (y < HdrH()) return 0;
            int row = RowAt(y);
            if (row < SelLo() || row > SelHi()) SetCursorRow(row, false);   // keep a selection the click is inside
            else { A.cursor = row; UpdateScroll(); UpdateStatus(); }
            HMENU pm = CreatePopupMenu();
            std::wstring lbl = (NoteAt(row).empty() ? L"Add note to frame " : L"Edit note of frame ") + std::to_wstring(row + 1) + L"...";
            bool hasNote = SelectionHasNote();
            std::wstring jump = L"Jump to frame " + std::to_wstring(row + 1) + L" (run forward, or rewind)";
            AppendMenuW(pm, MF_STRING | (A.busy ? MF_GRAYED : 0), IDM_JUMPCUR, jump.c_str());
            AppendMenuW(pm, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(pm, MF_STRING | (A.busy ? MF_GRAYED : 0), IDM_BM_EDIT, RowIsBm(row) ? L"Rename bookmark..." : L"Add bookmark here...");
            AppendMenuW(pm, MF_STRING | (A.busy || !RowIsBm(row) ? MF_GRAYED : 0), IDM_BM_DEL, L"Remove bookmark");
            AppendMenuW(pm, MF_STRING | (A.busy ? MF_GRAYED : 0), IDM_NOTE_EDIT, lbl.c_str());
            AppendMenuW(pm, MF_STRING | (A.busy || !hasNote ? MF_GRAYED : 0), IDM_NOTE_DEL, L"Remove note(s) in selection");
            AppendMenuW(pm, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(pm, MF_STRING, IDM_NOTE_LIST, L"Notes list...");
            AppendMenuW(pm, MF_STRING, IDM_BM_LIST, L"Bookmarks list...");
            POINT pt{x, y};
            ClientToScreen(h, &pt);
            TrackPopupMenu(pm, TPM_RIGHTBUTTON, pt.x, pt.y, 0, A.wnd, nullptr);
            DestroyMenu(pm);
            return 0;
        }
        case WM_LBUTTONDBLCLK:
            if (!A.busy && ColAt((short)LOWORD(lp)) < 0 && (short)HIWORD(lp) >= HdrH()) {
                SetCursorRow(RowAt((short)HIWORD(lp)), false);
                PostMessageW(A.wnd, WM_COMMAND, IDM_REWIND, 0);
            }
            return 0;
        case WM_MOUSEWHEEL:
            A.top -= GET_WHEEL_DELTA_WPARAM(wp) / 120 * 3;
            UpdateScroll();
            return 0;
        case WM_VSCROLL: {
            SCROLLINFO si{sizeof si, SIF_ALL};
            GetScrollInfo(h, SB_VERT, &si);
            switch (LOWORD(wp)) {
                case SB_LINEUP: A.top--; break;
                case SB_LINEDOWN: A.top++; break;
                case SB_PAGEUP: A.top -= si.nPage; break;
                case SB_PAGEDOWN: A.top += si.nPage; break;
                case SB_THUMBTRACK: case SB_THUMBPOSITION: A.top = si.nTrackPos; break;
            }
            UpdateScroll();
            return 0;
        }
        case WM_KEYDOWN: {
            bool shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
            int page = std::max(1, VisibleRows() - 1);
            switch (wp) {
                case VK_UP: SetCursorRow(A.cursor - 1, shift); break;
                case VK_DOWN: SetCursorRow(A.cursor + 1, shift); break;
                case VK_PRIOR: SetCursorRow(A.cursor - page, shift); break;
                case VK_NEXT: SetCursorRow(A.cursor + page, shift); break;
                case VK_HOME: SetCursorRow(0, shift); break;
                case VK_END: SetCursorRow(Size(), shift); break;
                case VK_DELETE: if (ctrl) EditDelete(); else EditClear(); break;
                case VK_INSERT: EditInsert(); break;
            }
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---- notes list window ---------------------------------------------------------
// Every note with its frame number; double-click or Enter jumps the cursor there,
// Delete removes the note.
void RefreshNotesList() {
    HWND lv = A.notesList;
    if (!lv) return;
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv);
    int n = 0;
    for (int r = 0; r < (int)A.movie.notes.size(); r++) {
        if (A.movie.notes[r].empty() || (A.listBm && !IsBm(A.movie.notes[r]))) continue;
        wchar_t b[16];
        swprintf(b, 16, L"%d", r + 1);
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = n++;
        it.pszText = b;
        it.lParam = r;
        int i = (int)SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
        std::wstring w = (IsBm(A.movie.notes[r]) ? L"\x2605 " : L"") + W(NoteBody(A.movie.notes[r]));
        ListView_SetItemText(lv, i, 1, (LPWSTR)w.c_str());
    }
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, nullptr, TRUE);
}

int NotesSelectedRow() {
    int i = ListView_GetNextItem(A.notesList, -1, LVNI_SELECTED);
    if (i < 0) return -1;
    LVITEMW it{};
    it.mask = LVIF_PARAM;
    it.iItem = i;
    return SendMessageW(A.notesList, LVM_GETITEMW, 0, (LPARAM)&it) ? (int)it.lParam : -1;
}

LRESULT CALLBACK NotesProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            if (A.notesList) MoveWindow(A.notesList, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
            return 0;
        case WM_NOTIFY: {
            NMHDR* nh = (NMHDR*)lp;
            if (nh->hwndFrom != A.notesList) break;
            if (nh->code == NM_DBLCLK || (nh->code == LVN_KEYDOWN && ((NMLVKEYDOWN*)lp)->wVKey == VK_RETURN)) {
                int row = NotesSelectedRow();
                if (row >= 0) { if (A.listBm) JumpTo(row); else SetCursorRow(row, false); }
            } else if (nh->code == LVN_KEYDOWN && ((NMLVKEYDOWN*)lp)->wVKey == VK_DELETE) {
                int row = NotesSelectedRow();
                if (row >= 0) { if (A.listBm) RemoveBookmark(row); else SetNote(row, RowIsBm(row) ? std::string(1, BM_FLAG) : std::string()); }
            }
            return 0;
        }
        case WM_CLOSE: DestroyWindow(h); return 0;
        case WM_DESTROY: A.notesWnd = A.notesList = nullptr; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void ShowNotesList(bool bookmarks = false) {
    if (A.notesWnd) {
        A.listBm = bookmarks;
        SetWindowTextW(A.notesWnd, bookmarks ? L"Bookmarks" : L"Frame notes");
        RefreshNotesList();
        SetForegroundWindow(A.notesWnd);
        return;
    }
    A.listBm = bookmarks;
    RECT pr;
    GetWindowRect(A.wnd, &pr);
    A.notesWnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"BscotmNotes", bookmarks ? L"Bookmarks" : L"Frame notes",
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_VISIBLE,
                                 pr.right - S(460), pr.top + S(90), S(440), S(360), A.wnd, nullptr, nullptr, nullptr);
    if (!A.notesWnd) return;
    A.notesList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                  0, 0, 0, 0, A.notesWnd, nullptr, nullptr, nullptr);
    SendMessageW(A.notesList, WM_SETFONT, (WPARAM)A.font, TRUE);
    ListView_SetExtendedListViewStyle(A.notesList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    LVCOLUMNW c{};
    c.mask = LVCF_TEXT | LVCF_WIDTH;
    c.pszText = (LPWSTR)L"Frame";
    c.cx = S(70);
    SendMessageW(A.notesList, LVM_INSERTCOLUMNW, 0, (LPARAM)&c);
    c.pszText = (LPWSTR)L"Note";
    c.cx = S(330);
    SendMessageW(A.notesList, LVM_INSERTCOLUMNW, 1, (LPARAM)&c);
    RECT cr;
    GetClientRect(A.notesWnd, &cr);
    MoveWindow(A.notesList, 0, 0, cr.right, cr.bottom, TRUE);
    RefreshNotesList();
}

// ---- game memory window -------------------------------------------------------
// Reads the running COTM.exe (any instance, frozen or live) with ReadProcessMemory every 100 ms.
// Pointer chains are from the community cheat table: dereference *(exe+root), then for each offset
// in reverse order dereference (p + off), and add the last one without dereferencing.
enum MemKind { MK_U8, MK_U32, MK_F32, MK_HEX, MK_BOSSHP, MK_BOSSMAX };   // the last two come from the boss finder, not a pointer chain
struct MemField { const wchar_t* name; uint32_t root; std::vector<uint32_t> xml; MemKind kind; };
const std::vector<MemField>& MemFields() {
    static const std::vector<uint32_t> pl = {0x84, 0x08, 0x20, 0x20, 0x6C, 0x08};
    auto P = [&](uint32_t field) { std::vector<uint32_t> v{field}; v.insert(v.end(), pl.begin(), pl.end()); return v; };
    static const std::vector<MemField> f = {
        {L"Health",          0x48365C, P(0x3DC), MK_U8},
        {L"Weapon points",   0x483660, {0x1E, 0x08}, MK_U8},
        {L"Max weapon points", 0x483660, {0x1D, 0x08}, MK_U8},
        {L"Score",           0x483660, {0x24, 0x08}, MK_U32},
        {L"X speed",         0x48365C, P(0x1A0), MK_F32},
        {L"Y speed",         0x48365C, P(0x1A4), MK_F32},
        {L"X position",      0x48365C, P(0x1AC), MK_F32},
        {L"Y position",      0x48365C, P(0x1B0), MK_F32},
        {L"X position (render)", 0x48365C, P(0x5B8), MK_F32},
        {L"Y position (render)", 0x48365C, P(0x5BC), MK_F32},
        {L"Invisibility",    0x48365C, P(0x53C), MK_F32},
        {L"Difficulty",      0x483660, {0x08, 0x08}, MK_U8},
        {L"Style",           0x483660, {0x0C, 0x08}, MK_U8},
        {L"Character 1",     0x483660, {0x11, 0x08}, MK_U8},
        {L"Character 2",     0x483660, {0x14, 0x08}, MK_U8},
        {L"Character 3",     0x483660, {0x17, 0x08}, MK_U8},
        {L"Character 4",     0x483660, {0x1A, 0x08}, MK_U8},
        {L"RNG state x",     0x48365C, {0x2F4}, MK_HEX},
        {L"RNG state y",     0x48365C, {0x2F8}, MK_HEX},
        {L"RNG state z",     0x48365C, {0x2FC}, MK_HEX},
        {L"RNG state w",     0x48365C, {0x300}, MK_HEX},
        {L"Boss HP",         0, {0}, MK_BOSSHP},
        {L"Boss max HP",     0, {0}, MK_BOSSMAX},
    };
    return f;
}

struct GameProc { DWORD pid = 0; HANDLE h = nullptr; uintptr_t base = 0; } gp;

bool AttachGame() {
    if (gp.h && WaitForSingleObject(gp.h, 0) == WAIT_TIMEOUT) return true;
    if (gp.h) { CloseHandle(gp.h); gp = GameProc(); }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{sizeof pe};
    DWORD pid = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (!_wcsicmp(pe.szExeFile, L"COTM.exe")) { pid = pe.th32ProcessID; break; }
    CloseHandle(snap);
    if (!pid) return false;
    HANDLE h = OpenProcess(PROCESS_VM_READ | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    uintptr_t base = 0;
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me{sizeof me};
        for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me))
            if (!_wcsicmp(me.szModule, L"COTM.exe")) { base = (uintptr_t)me.modBaseAddr; break; }
        CloseHandle(snap);
    }
    if (!base) { CloseHandle(h); return false; }
    gp.pid = pid; gp.h = h; gp.base = base;
    return true;
}

bool ReadU32(const GameProc& gp, uintptr_t a, uint32_t& v) {
    SIZE_T g = 0;
    return a && ReadProcessMemory(gp.h, (void*)a, &v, 4, &g) && g == 4;
}

// Returns the text for one field, or "-" if the chain cannot be followed (menu, loading, no stage).
// ---- boss finder ------------------------------------------------------------------------------------------
// The boss is not reachable from a fixed pointer chain, so a background thread looks for it in the game's
// heap: an object whose first dword is a known boss vtable (BOSS_VTABLES, RVAs in COTM.exe) and whose
// health fields look right (HP at +0x3DC, max HP at +0x3E0; the same layout as the player). It keeps the
// address while the object stays valid and scans again when it is gone (a new boss, a new launch).
// Found with a memory search during the stage 1 boss of test3.bscotm: HP 120, minus 7 per sub-weapon hit.
static const uint32_t BOSS_VTABLES[] = {0x39B99C};      // stage 1 boss
static const uint32_t BOSS_HP_OFF = 0x3DC, BOSS_MAX_OFF = 0x3E0;
static volatile uint32_t BossObj;                        // 0 = none found
static volatile LONG BossStop;
static HANDLE BossThread;
static bool OpenGameProc(GameProc& g, int tries = 100);

static bool BossValid(HANDLE h, uint32_t base, uint32_t obj) {
    uint32_t vt = 0, hp = 0, mx = 0;
    SIZE_T g;
    if (!ReadProcessMemory(h, (void*)(uintptr_t)obj, &vt, 4, &g) || g != 4) return false;
    bool known = false;
    for (uint32_t rva : BOSS_VTABLES) if (vt == base + rva) known = true;
    if (!known) return false;
    if (!ReadProcessMemory(h, (void*)(uintptr_t)(obj + BOSS_HP_OFF), &hp, 4, &g) || g != 4) return false;
    if (!ReadProcessMemory(h, (void*)(uintptr_t)(obj + BOSS_MAX_OFF), &mx, 4, &g) || g != 4) return false;
    return mx >= 2 && mx <= 100000 && hp <= mx;
}

static uint32_t BossScan(HANDLE h, uint32_t base) {
    std::vector<uint8_t> buf;
    for (uintptr_t a = 0; a < 0x7FFE0000 && !BossStop;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQueryEx(h, (void*)a, &mbi, sizeof mbi)) break;
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) && !(mbi.Protect & PAGE_GUARD)) {
            buf.resize(mbi.RegionSize);
            SIZE_T g = 0;
            if (ReadProcessMemory(h, mbi.BaseAddress, buf.data(), mbi.RegionSize, &g))
                for (size_t o = 0; o + 4 <= g; o += 4) {
                    uint32_t v;
                    memcpy(&v, &buf[o], 4);
                    for (uint32_t rva : BOSS_VTABLES)
                        if (v == base + rva && BossValid(h, base, (uint32_t)((uintptr_t)mbi.BaseAddress + o))) return (uint32_t)((uintptr_t)mbi.BaseAddress + o);
                }
        }
        a = next;
    }
    return 0;
}

static DWORD WINAPI BossProc(LPVOID) {
    HANDLE h = nullptr;
    uint32_t base = 0;
    DWORD pid = 0;
    while (!BossStop) {
        if (h && WaitForSingleObject(h, 0) != WAIT_TIMEOUT) { CloseHandle(h); h = nullptr; BossObj = 0; }
        if (!h) {
            GameProc probe;
            // find COTM.exe without keeping the memory window's handle: OpenGameProc opens its own
            if (OpenGameProc(probe, 1)) {
                pid = probe.pid;
                base = (uint32_t)probe.base;
                CloseHandle(probe.h);
                h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid);
            }
        }
        if (h) {
            uint32_t obj = BossObj;
            if (!obj || !BossValid(h, base, obj)) BossObj = obj = BossScan(h, base);
            Sleep(obj ? 250 : 700);
        } else {
            Sleep(500);
        }
    }
    if (h) CloseHandle(h);
    return 0;
}

static void StartBossFinder() {
    if (BossThread) return;
    BossStop = 0;
    BossThread = CreateThread(nullptr, 0, BossProc, nullptr, 0, nullptr);
}
static void StopBossFinder() {
    if (!BossThread) return;
    BossStop = 1;
    WaitForSingleObject(BossThread, 3000);
    CloseHandle(BossThread);
    BossThread = nullptr;
    BossObj = 0;
}
std::wstring ReadField(const GameProc& gp, const MemField& f) {
    if (f.kind == MK_BOSSHP || f.kind == MK_BOSSMAX) {            // from the boss finder
        uint32_t obj = BossObj, v = 0;
        if (!obj || !ReadU32(gp, obj + (f.kind == MK_BOSSHP ? BOSS_HP_OFF : BOSS_MAX_OFF), v)) return L"-";
        return std::to_wstring(v);
    }
    uint32_t p = 0;
    if (!ReadU32(gp, gp.base + f.root, p) || !p) return L"-";
    for (size_t i = f.xml.size() - 1; i > 0; i--)
        if (!ReadU32(gp, p + f.xml[i], p) || !p) return L"-";
    uintptr_t a = p + f.xml[0];
    wchar_t b[48];
    SIZE_T g = 0;
    if (f.kind == MK_U8) {
        uint8_t v;
        if (!ReadProcessMemory(gp.h, (void*)a, &v, 1, &g) || g != 1) return L"-";
        swprintf(b, 48, L"%u", (unsigned)v);
    } else {
        uint32_t v;
        if (!ReadU32(gp, a, v)) return L"-";
        if (f.kind == MK_U32) swprintf(b, 48, L"%u", (unsigned)v);
        else if (f.kind == MK_HEX) swprintf(b, 48, L"%08X", (unsigned)v);
        else {
            float fl;
            memcpy(&fl, &v, 4);
            if (fl != fl) return L"NaN";
            swprintf(b, 48, L"%.4f", (double)fl);
        }
    }
    return b;
}

// ---- value history (graph and CSV) -------------------------------------------------------------------
// The hook samples the player's values at every frame marker while the box is ticked; the editor copies the
// samples from the shared memory. Frames are movie frames: the state after that frame.
static Shm* OpenLogShm();
bool PickFile(bool save, const wchar_t* filter, const wchar_t* ext, std::wstring& io, const std::wstring& dir);
struct HistRow { int frame; uint32_t v[HIST_FIELDS]; };
static std::vector<HistRow> Hist;
static uint32_t HistLast;
static const wchar_t* const HIST_NAMES[HIST_FIELDS] = {L"Health", L"Weapon points", L"Score", L"X speed", L"Y speed", L"X position", L"Y position", L"Invisibility"};
static const bool HIST_FLOAT[HIST_FIELDS] = {false, false, false, true, true, true, true, true};

static bool HistValue(const HistRow& r, int f, double& out) {
    if (HIST_FLOAT[f]) {
        float x;
        memcpy(&x, &r.v[f], 4);
        if (x != x) return false;
        out = x;
        return true;
    }
    if (r.v[f] == 0xFFFFFFFFu) return false;
    out = r.v[f];
    return true;
}

void RefreshGraph();

void RefreshHistory() {
    Shm* s = OpenLogShm();
    if (!s) return;
    s->hist_on = A.histOn ? 1 : 0;
    uint32_t n = s->hist_n;
    if (n < HistLast) { Hist.clear(); HistLast = 0; }              // the game was relaunched: new history
    if (n - HistLast > HIST_MAX) HistLast = n - HIST_MAX;           // fell behind (old samples were overwritten)
    if (n == HistLast) return;
    for (uint32_t i = HistLast; i < n; i++) {
        const HistEntry& e = s->hist_buf[i & (HIST_MAX - 1)];
        int fr = (int)e.frame - 1 - A.logPre;                       // state after this movie frame
        if (fr < 1) continue;
        HistRow r;
        r.frame = fr;
        memcpy(r.v, e.v, sizeof r.v);
        Hist.push_back(r);
    }
    HistLast = n;
    if (Hist.size() > 400000) Hist.erase(Hist.begin(), Hist.begin() + 100000);
    RefreshGraph();
}

static void HistSaveCsv() {
    std::wstring p;
    if (!PickFile(true, L"CSV (*.csv)\0*.csv\0", L"csv", p, A.root)) return;
    FILE* f = _wfopen(p.c_str(), L"wb");
    if (!f) { MessageBoxW(A.wnd, L"Cannot write the file.", L"Value history", MB_ICONERROR); return; }
    fputs("frame", f);
    for (int i = 0; i < HIST_FIELDS; i++) { std::string n = U8(HIST_NAMES[i]); for (char& c : n) if (c == ' ') c = '_'; fprintf(f, ",%s", n.c_str()); }
    fputs("\r\n", f);
    for (const HistRow& r : Hist) {
        fprintf(f, "%d", r.frame);
        for (int i = 0; i < HIST_FIELDS; i++) {
            double v;
            if (!HistValue(r, i, v)) fputs(",", f);
            else if (HIST_FLOAT[i]) fprintf(f, ",%.4f", v);
            else fprintf(f, ",%.0f", v);
        }
        fputs("\r\n", f);
    }
    fclose(f);
    SetMsg(L"Saved " + std::to_wstring(Hist.size()) + L" frames of values to " + p);
}

// The graph: every selected value as a line over the frames, each scaled to the full height (the legend
// shows its range); move the mouse over the plot to read the values of one frame.
static int GraphMouseX = -1;
static const COLORREF GRAPH_COLORS[HIST_FIELDS] = {RGB(200, 40, 40), RGB(40, 120, 200), RGB(120, 120, 120), RGB(30, 150, 60),
                                                    RGB(220, 130, 20), RGB(130, 60, 190), RGB(20, 160, 170), RGB(170, 90, 90)};

static RECT GraphPlotRect(HWND h) {
    RECT rc;
    GetClientRect(h, &rc);
    rc.left = S(190) + S(8); rc.top = S(60); rc.right -= S(10); rc.bottom -= S(26);
    return rc;
}

void RefreshGraph() { if (A.graphWnd) InvalidateRect(A.graphWnd, nullptr, FALSE); }

static void PaintGraph(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT cr;
    GetClientRect(h, &cr);
    HDC m = CreateCompatibleDC(dc);
    HBITMAP bm = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
    HGDIOBJ old = SelectObject(m, bm);
    Fill(m, 0, 0, cr.right, cr.bottom, cBg);
    SelectObject(m, A.font);
    SetBkMode(m, TRANSPARENT);
    RECT pr = GraphPlotRect(h);
    int pw = pr.right - pr.left, ph = pr.bottom - pr.top;
    std::vector<int> sel;
    for (int i = 0; i < HIST_FIELDS; i++) if (SendMessageW(A.graphList, LB_GETSEL, i, 0) > 0) sel.push_back(i);
    HPEN frame = CreatePen(PS_SOLID, 1, cLine60);
    HGDIOBJ op = SelectObject(m, frame);
    MoveToEx(m, pr.left, pr.top, nullptr); LineTo(m, pr.left, pr.bottom); LineTo(m, pr.right, pr.bottom);
    SelectObject(m, op);
    DeleteObject(frame);
    if (Hist.empty() || sel.empty() || pw < 10 || ph < 10) {
        std::wstring msg = Hist.empty() ? L"No values yet. Tick \"Record history\" in the Memory window, then step or jump." : L"Select one or more values on the left.";
        Text(m, msg.c_str(), RECT{pr.left + S(10), pr.top + S(10), pr.right, pr.top + S(40)}, DT_LEFT, RGB(90, 90, 90));
    } else {
        int f0 = Hist.front().frame, f1 = Hist.back().frame;
        if (f1 <= f0) f1 = f0 + 1;
        wchar_t b[96];
        swprintf(b, 96, L"%d", f0);
        Text(m, b, RECT{pr.left, pr.bottom + S(4), pr.left + S(100), pr.bottom + S(22)}, DT_LEFT, RGB(60, 60, 60));
        swprintf(b, 96, L"frame %d", f1);
        Text(m, b, RECT{pr.right - S(120), pr.bottom + S(4), pr.right, pr.bottom + S(22)}, DT_RIGHT, RGB(60, 60, 60));
        int hover = -1;
        if (GraphMouseX >= pr.left && GraphMouseX <= pr.right) {
            int fr = f0 + (int)((double)(GraphMouseX - pr.left) / pw * (f1 - f0) + 0.5);
            size_t lo = 0, hi = Hist.size() - 1;
            while (lo < hi) { size_t mid = (lo + hi) / 2; if (Hist[mid].frame < fr) lo = mid + 1; else hi = mid; }
            hover = (int)lo;
        }
        int lx = pr.left + S(4);
        for (int f : sel) {
            double mn = 1e300, mx = -1e300;
            for (const HistRow& r : Hist) { double v; if (HistValue(r, f, v)) { mn = std::min(mn, v); mx = std::max(mx, v); } }
            if (mn > mx) continue;
            if (mx - mn < 1e-9) { mn -= 1; mx += 1; }
            HPEN pen = CreatePen(PS_SOLID, 2, GRAPH_COLORS[f]);
            HGDIOBJ op2 = SelectObject(m, pen);
            bool started = false;
            size_t step = std::max<size_t>(1, Hist.size() / (size_t)(pw * 2));
            for (size_t i = 0; i < Hist.size(); i += step) {
                double v;
                if (!HistValue(Hist[i], f, v)) { started = false; continue; }
                int x = pr.left + (int)((double)(Hist[i].frame - f0) / (f1 - f0) * pw);
                int y = pr.bottom - (int)((v - mn) / (mx - mn) * ph);
                if (!started) { MoveToEx(m, x, y, nullptr); started = true; } else LineTo(m, x, y);
            }
            SelectObject(m, op2);
            DeleteObject(pen);
            std::wstring name = HIST_NAMES[f];
            swprintf(b, 96, L"%ls  [%g .. %g]", name.c_str(), mn, mx);
            Fill(m, lx, S(8), lx + S(10), S(18), GRAPH_COLORS[f]);
            Text(m, b, RECT{lx + S(14), S(4), lx + S(260), S(22)}, DT_LEFT, RGB(30, 30, 30));
            lx += S(250);
            if (lx > cr.right - S(260)) lx = pr.left + S(4);
        }
        if (hover >= 0) {
            const HistRow& r = Hist[hover];
            int x = pr.left + (int)((double)(r.frame - f0) / (f1 - f0) * pw);
            HPEN hp = CreatePen(PS_SOLID, 1, RGB(120, 120, 120));
            HGDIOBJ o3 = SelectObject(m, hp);
            MoveToEx(m, x, pr.top, nullptr); LineTo(m, x, pr.bottom);
            SelectObject(m, o3);
            DeleteObject(hp);
            std::wstring line = L"frame " + std::to_wstring(r.frame);
            for (int f : sel) { double v; wchar_t t[64]; if (HistValue(r, f, v)) swprintf(t, 64, L"   %ls %g", HIST_NAMES[f], v); else swprintf(t, 64, L"   %ls -", HIST_NAMES[f]); line += t; }
            Text(m, line.c_str(), RECT{pr.left + S(4), S(28), cr.right - S(8), S(48)}, DT_LEFT, RGB(30, 30, 30));
        }
    }
    BitBlt(dc, 0, 0, cr.right, cr.bottom, m, 0, 0, SRCCOPY);
    SelectObject(m, old);
    DeleteObject(bm);
    DeleteDC(m);
    EndPaint(h, &ps);
}

LRESULT CALLBACK GraphProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            if (A.graphList) MoveWindow(A.graphList, 0, 0, S(190), HIWORD(lp), TRUE);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_COMMAND:
            if (HIWORD(wp) == LBN_SELCHANGE) InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE: GraphMouseX = (short)LOWORD(lp); InvalidateRect(h, nullptr, FALSE); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: PaintGraph(h); return 0;
        case WM_CLOSE: DestroyWindow(h); return 0;
        case WM_DESTROY: A.graphWnd = A.graphList = nullptr; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void ShowGraph() {
    if (A.graphWnd) { SetForegroundWindow(A.graphWnd); return; }
    RECT pr;
    GetWindowRect(A.wnd, &pr);
    A.graphWnd = CreateWindowExW(0, L"BscotmGraph", L"Value graph", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                 pr.left + S(80), pr.top + S(100), S(900), S(500), A.wnd, nullptr, nullptr, nullptr);
    if (!A.graphWnd) return;
    A.graphList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | LBS_MULTIPLESEL,
                                  0, 0, 0, 0, A.graphWnd, nullptr, nullptr, nullptr);
    SendMessageW(A.graphList, WM_SETFONT, (WPARAM)A.font, TRUE);
    for (int i = 0; i < HIST_FIELDS; i++) SendMessageW(A.graphList, LB_ADDSTRING, 0, (LPARAM)HIST_NAMES[i]);
    SendMessageW(A.graphList, LB_SETSEL, TRUE, 3);       // X speed and Y speed to start with
    SendMessageW(A.graphList, LB_SETSEL, TRUE, 4);
    RECT cr;
    GetClientRect(A.graphWnd, &cr);
    SendMessageW(A.graphWnd, WM_SIZE, 0, MAKELPARAM(cr.right, cr.bottom));
}
void RefreshMemory() {
    if (!A.memList) return;
    bool on = AttachGame();
    const auto& fs = MemFields();
    wchar_t cur[128];
    for (size_t i = 0; i < fs.size(); i++) {
        std::wstring v = on ? ReadField(gp, fs[i]) : L"-";
        ListView_GetItemText(A.memList, (int)i, 1, cur, 128);
        if (v != cur) ListView_SetItemText(A.memList, (int)i, 1, (LPWSTR)v.c_str());
    }
    SetWindowTextW(A.memWnd, on ? L"Game memory" : L"Game memory (game not running)");
    RefreshHistory();
}

LRESULT CALLBACK MemProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE: {
            int top = S(34);
            if (A.memList) MoveWindow(A.memList, 0, top, LOWORD(lp), std::max(0, (int)HIWORD(lp) - top), TRUE);
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_MEM_HIST: A.histOn = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED; RefreshHistory(); break;
                case IDC_MEM_GRAPH: ShowGraph(); break;
                case IDC_MEM_CSV: HistSaveCsv(); break;
                case IDC_MEM_CLR: Hist.clear(); RefreshGraph(); break;
            }
            return 0;
        case WM_TIMER: RefreshMemory(); return 0;
        case WM_CLOSE: DestroyWindow(h); return 0;
        case WM_DESTROY:
            KillTimer(h, 1);
            StopBossFinder();
            A.histOn = false;
            if (OpenLogShm()) OpenLogShm()->hist_on = 0;
            A.memWnd = A.memList = nullptr;
            if (gp.h) { CloseHandle(gp.h); gp = GameProc(); }
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void ShowMemory() {
    if (A.memWnd) { SetForegroundWindow(A.memWnd); return; }
    RECT pr;
    GetWindowRect(A.wnd, &pr);
    A.memWnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"BscotmMem", L"Game memory",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_VISIBLE,
                               pr.right - S(440), pr.top + S(60), S(420), S(700), A.wnd, nullptr, nullptr, nullptr);
    if (!A.memWnd) return;
    HWND hchk = CreateWindowExW(0, L"BUTTON", L"Record history", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                S(8), S(6), S(130), S(22), A.memWnd, (HMENU)(INT_PTR)IDC_MEM_HIST, nullptr, nullptr);
    HWND hgr = CreateWindowExW(0, L"BUTTON", L"Graph...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                               S(144), S(4), S(80), S(26), A.memWnd, (HMENU)(INT_PTR)IDC_MEM_GRAPH, nullptr, nullptr);
    HWND hcs = CreateWindowExW(0, L"BUTTON", L"Save CSV...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                               S(228), S(4), S(96), S(26), A.memWnd, (HMENU)(INT_PTR)IDC_MEM_CSV, nullptr, nullptr);
    HWND hcl = CreateWindowExW(0, L"BUTTON", L"Clear", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                               S(328), S(4), S(70), S(26), A.memWnd, (HMENU)(INT_PTR)IDC_MEM_CLR, nullptr, nullptr);
    for (HWND c : {hchk, hgr, hcs, hcl}) SendMessageW(c, WM_SETFONT, (WPARAM)A.font, TRUE);
    A.memList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER,
                                0, 0, 0, 0, A.memWnd, nullptr, nullptr, nullptr);
    SendMessageW(A.memList, WM_SETFONT, (WPARAM)A.font, TRUE);
    ListView_SetExtendedListViewStyle(A.memList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    LVCOLUMNW c{};
    c.mask = LVCF_TEXT | LVCF_WIDTH;
    c.pszText = (LPWSTR)L"Name";
    c.cx = S(150);
    SendMessageW(A.memList, LVM_INSERTCOLUMNW, 0, (LPARAM)&c);
    c.pszText = (LPWSTR)L"Value";
    c.cx = S(120);
    SendMessageW(A.memList, LVM_INSERTCOLUMNW, 1, (LPARAM)&c);
    const auto& fs = MemFields();
    for (size_t i = 0; i < fs.size(); i++) {
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = (LPWSTR)fs[i].name;
        SendMessageW(A.memList, LVM_INSERTITEMW, 0, (LPARAM)&it);
        ListView_SetItemText(A.memList, (int)i, 1, (LPWSTR)L"-");
    }
    RECT cr;
    GetClientRect(A.memWnd, &cr);
    SendMessageW(A.memWnd, WM_SIZE, 0, MAKELPARAM(cr.right, cr.bottom));
    RefreshMemory();
    StartBossFinder();
    SetTimer(A.memWnd, 1, 100, nullptr);
}

// ---- RNG log window ----------------------------------------------------------------------------------
// Every write to the game's RNG state (= every random draw) while the game runs the frames near the
// stopping point, as recorded by the hook (see RngLogEntry). The editor reads the hook's log straight from
// the shared memory, so it also shows draws made during frame advance and Run to cursor.
bool PickFile(bool save, const wchar_t* filter, const wchar_t* ext, std::wstring& io, const std::wstring& dir);
struct RngRow { uint32_t frame, eip, ret, range, w, kind; };
static std::vector<RngRow> RngRows;
static HANDLE LogMap;
static Shm* LogShm;
static uint32_t LogLast;

static Shm* OpenLogShm() {
    if (LogShm) return LogShm;
    LogMap = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, SHM_NAME);
    if (!LogMap) return nullptr;
    LogShm = (Shm*)MapViewOfFile(LogMap, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!LogShm || LogShm->magic != SHM_MAGIC) {
        if (LogShm) UnmapViewOfFile(LogShm);
        CloseHandle(LogMap);
        LogShm = nullptr; LogMap = nullptr;
    }
    return LogShm;
}

static std::wstring RngFrameText(const RngRow& r) {
    int fr = (int)r.frame - A.logPre;
    return fr >= 1 ? std::to_wstring(fr) : L"boot";
}
static void RngRowText(const RngRow& r, std::wstring t[6]) {
    wchar_t b[64];
    t[0] = RngFrameText(r);
    t[1] = r.kind == 1 ? L"draw" : r.kind == 2 ? L"draw (float)" : L"draw / write (inlined)";
    if (r.kind == 0) swprintf(b, 64, L"exe+%X  (called from exe+%X)", r.eip, r.ret);
    else swprintf(b, 64, L"exe+%X", r.ret);
    t[2] = b;
    if (r.kind == 1) {
        t[3] = std::to_wstring(r.range);
        t[4] = std::to_wstring(r.range ? r.w % r.range : r.w);
    } else { t[3] = L"-"; t[4] = L"-"; }
    swprintf(b, 64, L"%08X", r.w);
    t[5] = b;
}

static void RngAddRow(const RngRow& r) {
    std::wstring t[6];
    RngRowText(r, t);
    LVITEMW it{};
    it.mask = LVIF_TEXT;
    it.iItem = ListView_GetItemCount(A.rngList);
    it.pszText = (LPWSTR)t[0].c_str();
    int i = (int)SendMessageW(A.rngList, LVM_INSERTITEMW, 0, (LPARAM)&it);
    for (int c = 1; c < 6; c++) ListView_SetItemText(A.rngList, i, c, (LPWSTR)t[c].c_str());
}

static void RngClear() {
    RngRows.clear();
    if (A.rngList) ListView_DeleteAllItems(A.rngList);
}

void RefreshRngLog() {
    if (!A.rngList) return;
    Shm* s = OpenLogShm();
    if (!s) return;
    s->rng_log = A.rngLogOn ? 1 : 0;
    uint32_t n = s->rng_log_n;
    if (n < LogLast) { RngClear(); LogLast = 0; }            // the game was relaunched: new log
    if (n - LogLast > RNG_LOG_MAX) LogLast = n - RNG_LOG_MAX; // fell behind (old entries are overwritten)
    if (n == LogLast) return;
    SendMessageW(A.rngList, WM_SETREDRAW, FALSE, 0);
    for (uint32_t i = LogLast; i < n; i++) {
        const RngLogEntry& e = s->rng_log_buf[i & (RNG_LOG_MAX - 1)];
        RngRow r{e.frame, e.eip, e.ret, e.range, e.w, e.kind};
        RngRows.push_back(r);
        RngAddRow(r);
    }
    LogLast = n;
    while (RngRows.size() > 20000) { RngRows.erase(RngRows.begin()); ListView_DeleteItem(A.rngList, 0); }
    SendMessageW(A.rngList, WM_SETREDRAW, TRUE, 0);
    ListView_EnsureVisible(A.rngList, ListView_GetItemCount(A.rngList) - 1, FALSE);
    InvalidateRect(A.rngList, nullptr, TRUE);
}

static void RngSaveCsv() {
    std::wstring p;
    if (!PickFile(true, L"CSV (*.csv)\0*.csv\0", L"csv", p, A.root)) return;
    FILE* f = _wfopen(p.c_str(), L"wb");
    if (!f) { MessageBoxW(A.wnd, L"Cannot write the file.", L"RNG log", MB_ICONERROR); return; }
    fputs("frame,type,where,range,result,state_w\r\n", f);
    for (const RngRow& r : RngRows) {
        std::wstring t[6];
        RngRowText(r, t);
        std::string line;
        for (int c = 0; c < 6; c++) { std::string u = U8(t[c]); if (u.find(',') != std::string::npos) u = "\"" + u + "\""; line += (c ? "," : "") + u; }
        fputs((line + "\r\n").c_str(), f);
    }
    fclose(f);
    SetMsg(L"Saved " + std::to_wstring(RngRows.size()) + L" random draws to " + p);
}

LRESULT CALLBACK RngProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE: {
            int top = S(34);
            if (A.rngList) MoveWindow(A.rngList, 0, top, LOWORD(lp), std::max(0, (int)HIWORD(lp) - top), TRUE);
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_RNG_CHK: A.rngLogOn = SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED; RefreshRngLog(); break;
                case IDC_RNG_CLR: RngClear(); break;
                case IDC_RNG_CSV: RngSaveCsv(); break;
            }
            return 0;
        case WM_TIMER: RefreshRngLog(); return 0;
        case WM_CLOSE: DestroyWindow(h); return 0;
        case WM_DESTROY:
            KillTimer(h, 1);
            A.rngLogOn = false;
            if (LogShm) LogShm->rng_log = 0;
            A.rngWnd = A.rngList = nullptr;
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void ShowRngLog() {
    if (A.rngWnd) { SetForegroundWindow(A.rngWnd); return; }
    RECT pr;
    GetWindowRect(A.wnd, &pr);
    A.rngWnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"BscotmRng", L"RNG log",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_VISIBLE,
                               pr.left + S(60), pr.top + S(120), S(760), S(480), A.wnd, nullptr, nullptr, nullptr);
    if (!A.rngWnd) return;
    HWND chk = CreateWindowExW(0, L"BUTTON", L"Log the game's random draws", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                               S(8), S(6), S(210), S(22), A.rngWnd, (HMENU)(INT_PTR)IDC_RNG_CHK, nullptr, nullptr);
    HWND clr = CreateWindowExW(0, L"BUTTON", L"Clear", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                               S(230), S(4), S(70), S(26), A.rngWnd, (HMENU)(INT_PTR)IDC_RNG_CLR, nullptr, nullptr);
    HWND csv = CreateWindowExW(0, L"BUTTON", L"Save CSV...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                               S(306), S(4), S(100), S(26), A.rngWnd, (HMENU)(INT_PTR)IDC_RNG_CSV, nullptr, nullptr);
    for (HWND c : {chk, clr, csv}) SendMessageW(c, WM_SETFONT, (WPARAM)A.font, TRUE);
    A.rngList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                0, 0, 0, 0, A.rngWnd, nullptr, nullptr, nullptr);
    SendMessageW(A.rngList, WM_SETFONT, (WPARAM)A.font, TRUE);
    ListView_SetExtendedListViewStyle(A.rngList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    const wchar_t* names[6] = {L"Frame", L"Type", L"Where", L"Range", L"Result", L"State w"};
    int widths[6] = {60, 150, 190, 60, 90, 90};
    for (int c = 0; c < 6; c++) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = (LPWSTR)names[c];
        col.cx = S(widths[c]);
        SendMessageW(A.rngList, LVM_INSERTCOLUMNW, c, (LPARAM)&col);
    }
    RECT cr;
    GetClientRect(A.rngWnd, &cr);
    SendMessageW(A.rngWnd, WM_SIZE, 0, MAKELPARAM(cr.right, cr.bottom));
    RngClear();
    LogLast = LogShm ? LogShm->rng_log_n : 0;     // only draws from now on
    SetTimer(A.rngWnd, 1, 150, nullptr);
}
// ---- Help window ------------------------------------------------------------------------------------
// A guide for people who have not used the tool: topics on the left, text on the right. The text is part
// of the program so it is always there and always matches this version.
const wchar_t* const APP_VERSION = L"0.8.0";

struct HelpTopic { const wchar_t* title; const wchar_t* text; };
const HelpTopic HELP_TOPICS[] = {
    {L"Getting started",
     L"What this is\r\n"
     L"bscotm-tas is a TAStudio-style editor for Bloodstained: Curse of the Moon (Steam, version 1.1.2). A movie is a list of frames; each frame says which keys are held. The tool starts the game, feeds it the movie one frame at a time, and lets you step, rewind and re-record.\r\n\r\n"
     L"First run\r\n"
     L"1. Put bscotm_tas.exe and bscotm_hook.dll in the same folder (the DLL is injected into the game).\r\n"
     L"2. Start bscotm_tas.exe. It looks for Bloodstained Curse of the Moon\\exe\\COTM.exe in the folders above it. If it cannot find it, use File > Set game path.\r\n"
     L"3. Close the game if it is running. The tool starts its own copy and closes it when a new run starts.\r\n\r\n"
     L"Baseline and prelude\r\n"
     L"Baseline: a copy of the game's save files that is restored before every launch, so every run starts from the same state. Make one with File > Save current game saves as baseline (the game must be closed).\r\n"
     L"Prelude: the input that takes the game from launch to the first controllable frame (menus). It is played before your movie. Record it once, then File > Save as prelude.\r\n"
     L"Choose both in the two drop-downs at the top.\r\n\r\n"
     L"Your first movie\r\n"
     L"Press Record new (F8), click the game window and play. Press Stop (F9) when you are done, then File > Save (Ctrl+S).\r\n"
     L"To edit it: press Rewind to cursor (F6) to get the game to the cursor row, then step with the period key or paint keys in the grid."},
    {L"Recording and playing",
     L"Record new (F8): relaunch the game and record your keyboard from its first frame.\r\n"
     L"Play (F5): relaunch the game and play the whole movie in real time.\r\n"
     L"Rewind to cursor (F6, or double-click a frame number): relaunch, fast-forward to the cursor row and freeze the game there.\r\n"
     L"Record from cursor (F7): play to the cursor row, then record live from the next frame.\r\n"
     L"Record here (F12): unfreeze the game and record your keyboard from the frozen frame. Stop (F9) freezes it again.\r\n"
     L"Resume live (F11): unfreeze the game and take over with the keyboard (not recorded).\r\n"
     L"Stop (F9): end the current run.\r\n\r\n"
     L"Fast-forward\r\n"
     L"Rewind and the jumps run the game faster than real time. The speed is in Run > Fast-forward speed (default: Max, 24x). The game window may look frozen or garbled while it fast-forwards; audio is garbled too. Only the last frames before the stopping point are drawn.\r\n\r\n"
     L"The game window\r\n"
     L"The game accepts the tool's input even when the editor has focus. To type into the game yourself (Record here, Resume live), click the game window first."},
    {L"Frame advance and run",
     L"Frame advance (the period key): run exactly one frame of the frozen game using the next grid row's inputs. Hold the key to repeat. The game is held at a frame boundary before that frame reads its input, so you can still edit the row.\r\n\r\n"
     L"Run to cursor (F4, or right-click a row): advance the frozen game through many frames at once, up to the cursor row, using the grid's inputs. It runs at the fast-forward speed and gives the same result as stepping one by one. Stop (F9) cancels. Past the end of the movie it appends blank frames. If the game is already past the cursor, use Rewind.\r\n\r\n"
     L"Greenzone\r\n"
     L"Green frame numbers are frames the live game has been advanced through. Editing a frame before the game's position makes the later green frames invalid: the next advance first replays from the start to the edge of the green area, then steps.\r\n\r\n"
     L"Going back\r\n"
     L"The game cannot step backwards. To go back, move the cursor and Rewind (F6): it relaunches the game and fast-forwards to the cursor. A launch takes about 1.5 s; the replay runs at roughly 1800 frames per second."},
    {L"Editing frames",
     L"Painting: click or drag on a key cell to toggle it. Rows past the end extend the movie.\r\n"
     L"Selecting rows: click a frame number; Shift+click or drag to extend.\r\n"
     L"Delete clears inputs. Insert inserts blank frames. Ctrl+Delete removes frames.\r\n"
     L"Ctrl+C / Ctrl+X copy and cut. Ctrl+V pastes over; Ctrl+Shift+V pastes as inserted frames.\r\n"
     L"Ctrl+Z / Ctrl+Y undo and redo. Ctrl+A selects all.\r\n\r\n"
     L"Notes\r\n"
     L"Right-click a row (or use the Edit menu) to add a note to that frame. Notes show in the Note column and as an orange flag. Edit > Notes list opens every note; double-click jumps to it. Notes move with their frames on insert, delete, cut and paste and are saved in the movie file.\r\n\r\n"
     L"Finding\r\n"
     L"The grid has one column per key the tool tracks: arrows, A D W S, jump (space), attack (left mouse), sub-weapon (right mouse), Q, E, P, Enter and Esc."},
    {L"Bookmarks and jumping",
     L"A bookmark is a named frame you can jump back to. Ctrl+B (or right-click > Add bookmark here) names the cursor frame; it shows with a purple flag and a star. Ctrl+Shift+B removes it.\r\n\r\n"
     L"F2 / Shift+F2: jump to the next / previous bookmark.\r\n"
     L"Ctrl+G: jump to any frame number.\r\n"
     L"Edit > Bookmarks list: every bookmark; double-click or Enter jumps there, Delete removes it.\r\n\r\n"
     L"What a jump does\r\n"
     L"Forward (the frozen game has not reached the frame yet): the game just runs ahead at the fast-forward speed, no restart. Backward, or no game running: the game restarts and fast-forwards to the frame.\r\n\r\n"
     L"Bookmarks are frame notes with a flag, so they move with their frames, are covered by undo and are saved in the movie file."},
    {L"RNG seed and RNG log",
     L"How the game's randomness works\r\n"
     L"The game uses one random number generator, seeded from the clock (Unix time in seconds) when it starts. Launch the same movie one second later and the random numbers differ, so drops and enemy behaviour can differ.\r\n\r\n"
     L"RNG seed (Run > RNG seed)\r\n"
     L"Set a number from 0 to 4294967295 (avoid 0: it makes the generator stick at zero). The game then sees a frozen clock at that time, so every launch gets the same random numbers. The seed is saved in the movie. The status bar shows it. Changing it restarts the game on the next Rewind or advance. Seeds one second apart behave almost the same, so try seeds far apart.\r\n\r\n"
     L"RNG log (menu bar)\r\n"
     L"Opens a window that lists every random draw the game makes in the frames near where you stop (the last 300 frames of a Rewind, Run to cursor or Jump, and every frame you advance by hand). Tick Log the game's random draws, then step or jump as usual. Each row shows the frame, the code that made the draw, the range asked for (100 for an item-drop roll), the result and the generator state. A frame with no rows made no draws. Save CSV exports the list.\r\n\r\n"
     L"A seed fixes the random numbers at launch only. If you change your inputs, the game may draw a different number of values before an event, and that event's result changes too."},
    {L"Memory window",
     L"The Memory button in the menu bar opens a read-only window with the running game's values, 10 times a second: health, weapon points and maximum, score, X and Y speed, X and Y position (physics and the render copy), invisibility, difficulty, style, the four characters and the four RNG state words.\r\n\r\n"
     L"A dash means the game has no value yet (menus, loading). The window follows any COTM.exe that is running, frozen or live.\r\n\r\n"
     L"Value history\r\n"
     L"Tick Record history, then step, run or jump as usual: the game samples health, weapon points, score, X and Y speed, X and Y position and invisibility at every frame, also while fast-forwarding. Graph... draws the selected values over the frames (each value is scaled to the full height, its range is in the legend; move the mouse over the plot to read the values of one frame). Save CSV... writes every recorded frame; Clear empties the history. A new launch of the game (a rewind) starts a new history. Each row is the state after that movie frame."},
    {L"Verify fast-forward",
     L"Run > Verify fast-forward replays your whole movie twice at the fastest speed: once with every speed-up (no drawing, skipped render commands) and once with only the clock speed-up and normal drawing. It compares the game's values (the Memory window's list) at about 20 evenly spaced frames and tells you the first frame and value that differ, or that all matched.\r\n\r\n"
     L"Use it on your own movie, especially one that goes through other stages, before you trust a long fast-forward. Checkpoints during loading are skipped (loading runs on real time). The picture is not compared. A 10,000-frame movie takes about 40 seconds.\r\n\r\n"
     L"If it reports a difference, tell the developer which frame and value; some speed-up changes the game there."},
    {L"Keyboard shortcuts",
     L"File: Ctrl+N new, Ctrl+O open, Ctrl+S save, Ctrl+Shift+S save as\r\n"
     L"Edit: Ctrl+Z undo, Ctrl+Y redo, Ctrl+X cut, Ctrl+C copy, Ctrl+V paste, Ctrl+Shift+V paste insert, Ctrl+A select all, Delete clear, Insert insert frames, Ctrl+Delete delete frames, Ctrl+R repeat selection, Ctrl+Shift+R fill pattern, Ctrl+F find, F3 / Shift+F3 find next / previous, Alt+Down / Alt+Up next / previous note\r\n"
     L"Bookmarks: Ctrl+B add / rename, Ctrl+Shift+B remove, F2 next, Shift+F2 previous, Ctrl+G jump to frame\r\n"
     L"Run: F5 play, F6 rewind to cursor, F7 record from cursor, F8 record new, F9 stop, F4 run to cursor, period frame advance, F11 resume live, F12 record here\r\n"
     L"Help: F1 this guide"},
    {L"Troubleshooting",
     L"The tool says it cannot find the game\r\n"
     L"Use File > Set game path and pick COTM.exe.\r\n\r\n"
     L"bscotm_hook.dll must be next to bscotm_tas.exe\r\n"
     L"Keep the two files together; unzip the release into one folder.\r\n\r\n"
     L"The game exits right away or the tool cannot start it\r\n"
     L"Close any running copy of the game first, and run the tool from a folder you can write to. Some antivirus programs block the injected DLL; add an exception for the folder.\r\n\r\n"
     L"The movie plays differently from last time\r\n"
     L"Set an RNG seed (random drops and enemies depend on the launch second), use the same baseline and prelude, and run Verify fast-forward to check the speed-ups.\r\n\r\n"
     L"The game picture is black or wrong after a fast jump\r\n"
     L"Step one frame or press Rewind (F6) again. The picture is only drawn for the last frames before a stop.\r\n\r\n"
     L"Keys typed into the editor appear in the game, or the reverse\r\n"
     L"Live keys only count while the game window is in front. If you use Record here or Resume live, click the game window first."},
    {L"About",
     nullptr},
};
const int NUM_HELP_TOPICS = (int)(sizeof HELP_TOPICS / sizeof HELP_TOPICS[0]);

HWND helpWnd = nullptr, helpList = nullptr, helpText = nullptr;

void ShowHelpTopic(int i) {
    if (!helpText || i < 0 || i >= NUM_HELP_TOPICS) return;
    std::wstring t;
    if (HELP_TOPICS[i].text) t = HELP_TOPICS[i].text;
    else t = std::wstring(L"bscotm-tas ") + APP_VERSION + L"\r\n\r\nA TAStudio-style frame editor for Bloodstained: Curse of the Moon (Steam, version 1.1.2).\r\n\r\n"
             L"Project: https://github.com/Cheetah555V2/bscotm_tas\r\nReleases: https://github.com/Cheetah555V2/bscotm_tas/releases\r\n"
             L"License: MIT.\r\n\r\nThe tool injects bscotm_hook.dll into the game. It patches only the game's import tables and data, never its code. How it works and every finding: docs\\REVERSE_ENGINEERING.md in the repository.";
    SetWindowTextW(helpText, t.c_str());
    SendMessageW(helpText, EM_SETSEL, 0, 0);
    SendMessageW(helpText, EM_SCROLLCARET, 0, 0);
    SendMessageW(helpList, LB_SETCURSEL, i, 0);
}

LRESULT CALLBACK HelpProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE: {
            int lw = S(190), w = LOWORD(lp), ht = HIWORD(lp);
            if (helpList) MoveWindow(helpList, 0, 0, lw, ht, TRUE);
            if (helpText) MoveWindow(helpText, lw, 0, std::max(0, w - lw), ht, TRUE);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == IDC_HELP_TOPICS && HIWORD(wp) == LBN_SELCHANGE)
                ShowHelpTopic((int)SendMessageW(helpList, LB_GETCURSEL, 0, 0));
            return 0;
        case WM_CLOSE: DestroyWindow(h); return 0;
        case WM_DESTROY: helpWnd = helpList = helpText = nullptr; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void ShowHelp(int topic) {
    if (!helpWnd) {
        RECT pr;
        GetWindowRect(A.wnd, &pr);
        helpWnd = CreateWindowExW(0, L"BscotmHelp", L"bscotm-tas Help",
                                  WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                  pr.left + S(50), pr.top + S(60), S(860), S(560), A.wnd, nullptr, nullptr, nullptr);
        if (!helpWnd) return;
        helpList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | WS_VSCROLL,
                                   0, 0, 0, 0, helpWnd, (HMENU)(INT_PTR)IDC_HELP_TOPICS, nullptr, nullptr);
        helpText = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
                                   0, 0, 0, 0, helpWnd, nullptr, nullptr, nullptr);
        LOGFONTW lf{};
        GetObjectW(A.font, sizeof lf, &lf);
        lf.lfHeight = lf.lfHeight * 12 / 10;
        static HFONT big = CreateFontIndirectW(&lf);
        SendMessageW(helpList, WM_SETFONT, (WPARAM)big, TRUE);
        SendMessageW(helpText, WM_SETFONT, (WPARAM)big, TRUE);
        for (const HelpTopic& t : HELP_TOPICS) SendMessageW(helpList, LB_ADDSTRING, 0, (LPARAM)t.title);
        RECT cr;
        GetClientRect(helpWnd, &cr);
        SendMessageW(helpWnd, WM_SIZE, 0, MAKELPARAM(cr.right, cr.bottom));
    }
    ShowHelpTopic(topic);
    ShowWindow(helpWnd, SW_SHOWNORMAL);
    SetForegroundWindow(helpWnd);
}
// ---- files, paths, config ---------------------------------------------------
bool PickFile(bool save, const wchar_t* filter, const wchar_t* ext, std::wstring& io, const std::wstring& dir) {
    wchar_t buf[MAX_PATH * 2] = {};
    wcsncpy(buf, io.c_str(), MAX_PATH * 2 - 1);
    OPENFILENAMEW o{};
    o.lStructSize = sizeof o;
    o.hwndOwner = A.wnd;
    o.lpstrFilter = filter;
    o.lpstrFile = buf;
    o.nMaxFile = MAX_PATH * 2;
    o.lpstrDefExt = ext;
    o.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    o.Flags = OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&o) : GetOpenFileNameW(&o))) return false;
    io = buf;
    return true;
}

bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring IniGet(const wchar_t* sec, const wchar_t* key) {
    wchar_t b[MAX_PATH * 2];
    GetPrivateProfileStringW(sec, key, L"", b, MAX_PATH * 2, A.ini.c_str());
    return b;
}
void IniSet(const wchar_t* sec, const wchar_t* key, const std::wstring& v) {
    WritePrivateProfileStringW(sec, key, v.c_str(), A.ini.c_str());
}

void ResolvePaths() {
    wchar_t b[MAX_PATH];
    GetModuleFileNameW(nullptr, b, MAX_PATH);
    A.toolDir = b;
    A.toolDir.resize(A.toolDir.find_last_of(L'\\'));
    A.ini = A.toolDir + L"\\bscotm_tas.ini";

    // Walk up looking for "<dir>\Bloodstained Curse of the Moon\exe\COTM.exe".
    std::wstring dir = A.toolDir, found;
    for (int i = 0; i < 5 && found.empty(); i++) {
        if (Exists(dir + L"\\Bloodstained Curse of the Moon\\exe\\COTM.exe")) found = dir;
        size_t p = dir.find_last_of(L'\\');
        if (p == std::wstring::npos) break;
        dir.resize(p);
    }
    A.exe = IniGet(L"paths", L"game");
    if (!Exists(A.exe)) A.exe = found.empty() ? L"" : found + L"\\Bloodstained Curse of the Moon\\exe\\COTM.exe";
    A.root = IniGet(L"paths", L"data");
    if (A.root.empty()) A.root = found.empty() ? A.toolDir : found;
}

bool EnsureGamePath(bool force = false) {
    if (!force && Exists(A.exe)) return true;
    std::wstring p = A.exe;
    if (!PickFile(false, L"COTM.exe\0COTM.exe\0All files\0*.*\0", nullptr, p, L"")) return false;
    A.exe = p;
    IniSet(L"paths", L"game", p);
    return true;
}

std::wstring ComboSel(HWND cb) {
    int i = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
    if (i <= 0) return L"";
    wchar_t b[260];
    SendMessageW(cb, CB_GETLBTEXT, i, (LPARAM)b);
    return b;
}

void ComboFill(HWND cb, const std::wstring& pattern, bool dirs, const wchar_t* strip, const std::wstring& select) {
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"(none)");
    int sel = 0;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pattern.c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (isDir != dirs || fd.cFileName[0] == L'.') continue;
            std::wstring n = fd.cFileName;
            if (strip && n.size() > wcslen(strip)) n.resize(n.size() - wcslen(strip));
            int i = (int)SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)n.c_str());
            if (n == select) sel = i;
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    SendMessageW(cb, CB_SETCURSEL, sel, 0);
}

void RefreshCombos(const std::wstring& baseSel, const std::wstring& preSel) {
    ComboFill(A.cbBase, A.root + L"\\save_backups\\baselines\\*", true, nullptr, baseSel);
    ComboFill(A.cbPre, A.root + L"\\preludes\\*.bscotm", false, L".bscotm", preSel);
}

bool ConfirmDiscard();

void LoadPath(const std::wstring& p) {
    Movie m;
    std::string err;
    if (!m.Load(p, err)) { MessageBoxW(A.wnd, W(err).c_str(), L"Open failed", MB_ICONERROR); return; }
    A.movie = std::move(m);
    A.path = p;
    A.dirty = false;
    A.undo.clear(); A.redo.clear();
    A.cursor = A.anchor = A.top = A.reached = 0;
    if (!A.movie.prelude_id.empty()) {
        int i = (int)SendMessageW(A.cbPre, CB_FINDSTRINGEXACT, 0, (LPARAM)W(A.movie.prelude_id).c_str());
        if (i >= 0) SendMessageW(A.cbPre, CB_SETCURSEL, i, 0);
    }
    UpdateTitle(); UpdateScroll(); UpdateStatus(); RefreshNotesList();
    SetMsg(L"Loaded " + p);
}

bool WriteMovie(const std::wstring& p, const std::wstring& preludeId = L"") {
    if (A.movie.author.empty()) {
        wchar_t u[256]; DWORD n = 256;
        GetUserNameW(u, &n);
        A.movie.author = U8(u);
    }
    if (A.movie.created.empty()) {
        SYSTEMTIME t; GetSystemTime(&t);
        char b[32];
        snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02dZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        A.movie.created = b;
    }
    Movie m = A.movie;
    std::wstring pre = preludeId.empty() ? ComboSel(A.cbPre) : preludeId;
    m.prelude_id = U8(pre);
    std::string err;
    if (!m.Save(p, err)) { MessageBoxW(A.wnd, W(err).c_str(), L"Save failed", MB_ICONERROR); return false; }
    return true;
}

bool SaveAs() {
    std::wstring p = A.path;
    if (!PickFile(true, L"bscotm-tas movie (*.bscotm)\0*.bscotm\0All files\0*.*\0", L"bscotm", p, A.root)) return false;
    if (!WriteMovie(p)) return false;
    A.path = p; A.dirty = false; UpdateTitle();
    SetMsg(L"Saved " + p);
    return true;
}

bool Save() {
    if (A.path.empty()) return SaveAs();
    if (!WriteMovie(A.path)) return false;
    A.dirty = false; UpdateTitle();
    SetMsg(L"Saved " + A.path);
    return true;
}

bool ConfirmDiscard() {
    if (!A.dirty) return true;
    int r = MessageBoxW(A.wnd, L"Save changes to the current movie?", L"bscotm-tas", MB_YESNOCANCEL | MB_ICONQUESTION);
    return r == IDNO || (r == IDYES && Save());
}

void FileNew() {
    if (A.busy || !ConfirmDiscard()) return;
    A.movie = Movie();
    A.path.clear(); A.dirty = false;
    A.undo.clear(); A.redo.clear();
    A.cursor = A.anchor = A.top = A.reached = 0;
    UpdateTitle(); UpdateScroll(); UpdateStatus(); RefreshNotesList();
}

void FileOpen() {
    if (A.busy || !ConfirmDiscard()) return;
    std::wstring p;
    if (PickFile(false, L"bscotm-tas movie (*.bscotm)\0*.bscotm\0All files\0*.*\0", L"bscotm", p, A.root)) LoadPath(p);
}

void SavePrelude() {
    std::wstring dir = A.root + L"\\preludes", p;
    CreateDirectoryW(dir.c_str(), nullptr);
    if (!PickFile(true, L"Prelude (*.bscotm)\0*.bscotm\0", L"bscotm", p, dir)) return;
    std::wstring id = p.substr(p.find_last_of(L"\\/") + 1);
    id.resize(id.size() > 7 ? id.size() - 7 : id.size());     // strip ".bscotm"
    if (!WriteMovie(p, id)) return;
    RefreshCombos(ComboSel(A.cbBase), id);
    SetMsg(L"Prelude saved: " + id);
}

void BaselineSave() {
    if (A.busy) return;
    if (!EnsureGamePath()) return;
    std::wstring dir = A.root + L"\\save_backups\\baselines", p;
    CreateDirectoryW((A.root + L"\\save_backups").c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    if (!PickFile(true, L"Baseline name\0*.*\0", nullptr, p, dir)) return;
    std::wstring id = p.substr(p.find_last_of(L"\\/") + 1);
    std::string err;
    if (!SaveBaseline(A.exe, dir + L"\\" + id, err)) { MessageBoxW(A.wnd, W(err).c_str(), L"Baseline", MB_ICONWARNING); return; }
    RefreshCombos(id, ComboSel(A.cbPre));
    SetMsg(L"Baseline saved: " + id);
}

// ---- jobs (play / rewind / record) run on a worker thread -------------------
void EnableUi();

void JobProgress(void*, Phase ph, uint32_t f) { PostMessageW(A.wnd, WM_JOB_PROGRESS, ph, f); }

struct JobArgs { RunParams p; };

DWORD WINAPI JobThread(LPVOID a) {
    JobArgs* j = (JobArgs*)a;
    RunCallbacks cb;
    cb.progress = JobProgress;
    cb.stop = &A.stop;
    RunResult* r = new RunResult(RunJob(j->p, cb, j->p.hold ? &A.jobSess : nullptr));
    delete j;
    PostMessageW(A.wnd, WM_JOB_DONE, 0, (LPARAM)r);
    return 0;
}

const uint32_t SPEEDS[4] = {1000, 2000, 8000, 24000};     // 24x is the fastest speed at which the game plays out exactly as in real time (32x: about 1 run in 4 is off by a frame, 50x: 1 in 2; see docs)

// The parts of a run that do not depend on what it is for: game, hook, saves, prelude and RNG seed.
bool FillParams(RunParams& p) {
    p.exe = A.exe;
    p.dll = A.toolDir + L"\\bscotm_hook.dll";
    std::wstring base = ComboSel(A.cbBase), pre = ComboSel(A.cbPre);
    if (!base.empty()) p.baseline = A.root + L"\\save_backups\\baselines\\" + base;
    p.backup_dir = A.root + L"\\save_backups\\last_user_state";
    if (!A.movie.prelude.empty()) {
        p.prelude = A.movie.prelude;
    } else if (!pre.empty()) {
        Movie pm;
        std::string err;
        if (!pm.Load(A.root + L"\\preludes\\" + pre + L".bscotm", err)) {
            MessageBoxW(A.wnd, W("Prelude: " + err).c_str(), L"bscotm-tas", MB_ICONERROR);
            return false;
        }
        p.prelude = pm.frames;
    }
    p.seeded = A.movie.has_seed;
    p.seed = A.movie.seed;
    return true;
}
// realtime: ignore the fast-forward setting (watching a movie, not seeking).
// hold: leave the game frozen after `target` frames so it can be stepped.
void StartJob(bool record, uint32_t target, bool fresh, bool realtime = false, bool hold = false) {
    if (A.busy || !EnsureGamePath()) return;
    if (!record && !Size()) { MessageBoxW(A.wnd, L"The movie is empty.", L"bscotm-tas", MB_ICONINFORMATION); return; }
    if (!A.exe.empty() && !Exists(A.toolDir + L"\\bscotm_hook.dll")) {
        MessageBoxW(A.wnd, L"bscotm_hook.dll must be next to bscotm_tas.exe.", L"bscotm-tas", MB_ICONERROR);
        return;
    }
    A.sess.Close();         // the job kills the game; drop our handles first
    JobArgs* j = new JobArgs;
    RunParams& p = j->p;
    if (!FillParams(p)) { delete j; return; }
    p.rng_log = A.rngWnd && A.rngLogOn;
    p.hist = A.memWnd && A.histOn;
    A.logPre = (int)p.prelude.size();
    if (!fresh) p.movie = A.movie.frames;
    p.target = target;
    p.record = record;
    p.speed_milli = realtime ? 1000 : SPEEDS[A.speed];
    // The virtual clock is always on (identical to real time at 1x) so a frozen game
    // can be thawed without a burst of catch-up frames.
    p.speed_mask = p.speed_milli > 1000 ? SPEED_ALL : (SPEED_ALL & ~SPEED_NOVSYNC);
    p.hold = hold;
    p.savestates = A.useStates && hold;

    A.jobRecord = record; A.jobNew = fresh; A.jobTarget = target; A.jobHold = hold;
    A.busy = true;
    A.stop = 0;
    EnableUi();
    SetMsg(L"Launching the game...");
    A.thread = CreateThread(nullptr, 0, JobThread, j, 0, nullptr);
}

// ---- verify fast-forward ------------------------------------------------------------------------
// Replays the movie twice at the fastest speed, once with all speed-ups (no drawing, render-command
// skip) and once with only the clock speed-up and normal drawing, and compares the game's values
// (the Memory window's list) at evenly spaced frames. A difference means a speed-up changes the game.
const UINT WM_VERIFY_DONE = WM_APP + 3;       // lParam = std::wstring* (the report)
const UINT WM_VERIFY_PROGRESS = WM_APP + 4;   // lParam = std::wstring* (a status line)

struct VerifyArgs { RunParams p; std::vector<int> cps; };

struct VerifyRun {
    std::vector<std::vector<std::wstring>> vals;      // [checkpoint][field]
    std::string error;
};

static void VerifyNote(const std::wstring& t) { PostMessageW(A.wnd, WM_VERIFY_PROGRESS, 0, (LPARAM)new std::wstring(t)); }

static bool OpenGameProc(GameProc& g, int tries) {            // like AttachGame, but for the worker thread's own handle
    for (int i = 0; i < tries && !g.h; i++) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W pe{sizeof pe};
        DWORD pid = 0;
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
            if (!_wcsicmp(pe.szExeFile, L"COTM.exe")) { pid = pe.th32ProcessID; break; }
        CloseHandle(snap);
        if (pid) {
            HANDLE h = OpenProcess(PROCESS_VM_READ | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            uintptr_t base = 0;
            snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
            if (h && snap != INVALID_HANDLE_VALUE) {
                MODULEENTRY32W me{sizeof me};
                for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me))
                    if (!_wcsicmp(me.szModule, L"COTM.exe")) { base = (uintptr_t)me.modBaseAddr; break; }
                CloseHandle(snap);
            }
            if (h && base) { g.pid = pid; g.h = h; g.base = base; return true; }
            if (h) CloseHandle(h);
        }
        Sleep(100);
    }
    return false;
}

static bool VerifyPass(const VerifyArgs& va, uint32_t mask, const wchar_t* label, VerifyRun& out) {
    RunParams p = va.p;
    p.speed_milli = 24000;
    p.speed_mask = mask;
    p.hold = true;
    p.record = false;
    p.target = 1;
    RunCallbacks cb;
    cb.stop = &A.stop;
    Session ss;
    VerifyNote(std::wstring(label) + L": launching the game...");
    RunResult rr = RunJob(p, cb, &ss);
    if (!rr.ok || !ss.Active()) { out.error = rr.error.empty() ? "the game did not start" : rr.error; return false; }
    GameProc g;
    if (!OpenGameProc(g)) { out.error = "could not read the game's memory"; ss.Close(); KillGame(); return false; }
    const auto& fs = MemFields();
    int pos = 1;
    for (int cp : va.cps) {
        if (cp > pos) {
            if (!ss.BeginSteps(p.movie.data() + pos, (uint32_t)(cp - pos), 24000)) { out.error = "could not run the game"; break; }
            int r;
            while (!(r = ss.PollSteps())) {
                if (A.stop) ss.AbortSteps();
                Sleep(5);
            }
            if (r < 0 || A.stop) { out.error = A.stop ? "Stopped." : "the game stopped responding"; break; }
            pos = cp;
        }
        std::vector<std::wstring> row;
        for (const MemField& f : fs) row.push_back(f.kind >= MK_BOSSHP ? std::wstring(L"-") : ReadField(g, f));
        out.vals.push_back(std::move(row));
        wchar_t b[96];
        swprintf(b, 96, L"%ls: frame %d / %d", label, cp, va.cps.back());
        VerifyNote(b);
    }
    CloseHandle(g.h);
    ss.Close();
    KillGame();
    return out.error.empty();
}

DWORD WINAPI VerifyThread(LPVOID a) {
    VerifyArgs* va = (VerifyArgs*)a;
    VerifyRun slow, fast;
    std::wstring report;
    bool ok = VerifyPass(*va, SPEED_ALL & ~(SPEED_NODRAW | SPEED_NORENDER), L"Reference run (drawing on)", slow);
    if (ok) ok = VerifyPass(*va, SPEED_ALL, L"Fast run (all speed-ups)", fast);
    if (!ok) {
        const std::string& e = !slow.error.empty() ? slow.error : fast.error;
        report = e == "Stopped." ? L"Verification stopped." : L"Verification failed: " + W(e);
    } else {
        const auto& fs = MemFields();
        // While the game is still starting or loading (no player yet) its background loading runs on real
        // time, so those checkpoints differ between any two runs: they are skipped.
        int bad = -1, badField = -1, compared = 0, skipped = 0, first = 0, last = 0;
        for (size_t c = 0; c < va->cps.size() && bad < 0; c++) {
            if (slow.vals[c][0] == L"-" || fast.vals[c][0] == L"-") { skipped++; continue; }
            if (!compared) first = va->cps[c];
            compared++;
            last = va->cps[c];
            for (size_t i = 0; i < fs.size(); i++)
                if (slow.vals[c][i] != fast.vals[c][i]) { bad = (int)c; badField = (int)i; break; }
        }
        wchar_t b[384];
        if (bad < 0 && !compared) {
            report = L"No checkpoint could be compared: the game had no player at any of them (still loading or in the menus).";
        } else if (bad < 0) {
            swprintf(b, 384, L"The fast run matches the reference run at all %d compared checkpoints (frames %d to %d): %zu game values each (health, weapon points, speeds, positions, RNG state, ...).%ls\n\nThe picture is not compared.",
                     compared, first, last, fs.size(),
                     skipped ? (std::wstring(L"\n") + std::to_wstring(skipped) + L" earlier checkpoint(s) were skipped because the game was still loading.").c_str() : L"");
            report = b;
        } else {
            swprintf(b, 256, L"DIFFERENT at frame %d: %ls\n  reference run: %ls\n  fast run:      %ls\n\nSome speed-up changes the game here. First look at the frames just before this checkpoint.",
                     va->cps[bad], fs[badField].name, slow.vals[bad][badField].c_str(), fast.vals[bad][badField].c_str());
            report = b;
        }
    }
    delete va;
    PostMessageW(A.wnd, WM_VERIFY_DONE, 0, (LPARAM)new std::wstring(report));
    return 0;
}

void VerifyFastForward() {
    if (A.busy || !EnsureGamePath()) return;
    if (Size() < 2) { MessageBoxW(A.wnd, L"The movie is too short to verify.", L"Verify fast-forward", MB_ICONINFORMATION); return; }
    if (!A.exe.empty() && !Exists(A.toolDir + L"\\bscotm_hook.dll")) {
        MessageBoxW(A.wnd, L"bscotm_hook.dll must be next to bscotm_tas.exe.", L"bscotm-tas", MB_ICONERROR);
        return;
    }
    int n = Size();
    int gap = std::max(200, n / 20);
    wchar_t msg[512];
    swprintf(msg, 512, L"This replays the whole movie (%d frames) twice at the fastest speed, once with every speed-up and once with only the clock speed-up and normal drawing, and compares the game's values at about %d checkpoints.\n\nIt takes roughly %d minutes and the game window will be in front. Continue?",
             n, (n + gap - 1) / gap, std::max(1, (int)(n / 340.0 / 60.0 + n / 1100.0 / 60.0 + 1.5)));
    if (MessageBoxW(A.wnd, msg, L"Verify fast-forward", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    A.sess.Close();
    VerifyArgs* va = new VerifyArgs;
    if (!FillParams(va->p)) { delete va; return; }
    va->p.movie = A.movie.frames;
    for (int c = gap; c < n; c += gap) va->cps.push_back(c);
    va->cps.push_back(n);
    A.reached = 0;
    A.busy = true;
    A.stop = 0;
    EnableUi();
    SetMsg(L"Verifying...");
    A.thread = CreateThread(nullptr, 0, VerifyThread, va, 0, nullptr);
}

void OnVerifyDone(std::wstring* report) {
    if (A.thread) { WaitForSingleObject(A.thread, 2000); CloseHandle(A.thread); A.thread = nullptr; }
    A.busy = false;
    EnableUi();
    UpdateScroll(); UpdateStatus();
    SetMsg(report->compare(0, 4, L"DIFF") == 0 ? L"Verify fast-forward: DIFFERENT (see the message)." : L"Verify fast-forward finished.");
    MessageBoxW(A.wnd, report->c_str(), L"Verify fast-forward", MB_OK | (report->compare(0, 4, L"DIFF") == 0 ? MB_ICONWARNING : MB_ICONINFORMATION));
    delete report;
    SetFocus(A.grid);
}
void OnJobProgress(Phase ph, uint32_t f) {
    wchar_t b[96];
    switch (ph) {
        case PH_LAUNCH: SetMsg(L"Launching the game..."); break;
        case PH_BOOT: SetMsg(L"Starting the game..."); break;
        case PH_PLAY:
            swprintf(b, 96, A.jobRecord ? L"Replaying to the cursor: frame %u / %u" : L"Playing: frame %u / %u", f, A.jobTarget);
            SetMsg(b);
            if (f && (int)f - 1 <= Size()) SetCursorRow((int)f - 1, false);
            break;
        case PH_RECORD:
            swprintf(b, 96, L"RECORDING - frame %u. Play now, press Stop (F9) when done.", A.jobTarget + f);
            SetMsg(b);
            break;
    }
}

void FrameAdvance();
void RunToCursor();
void StopRecordHere();

// ---- savestates --------------------------------------------------------------------------------
// A state restores "the game frozen before movie row R". It is valid while no frame before R was edited
// (R <= A.reached). Rewinding loads the latest valid state at or before the target instead of relaunching.
bool StatesOn() {
    return A.useStates && A.sess.Active() && A.sess.Alive() && A.sess.s && (A.sess.s->features & FEAT_ARENA_OK);
}

int NumStates() {
    int n = 0;
    if (A.sess.Active() && A.sess.s) for (int i = 0; i < 8; i++) { int r = A.sess.StateRow(i); if (r >= 0 && r <= A.reached) n++; }
    return n;
}

// The slot to rewind from for a game that has to run row `next` next: the latest valid state at or before it.
int BestState(int next) {
    int best = -1, bestRow = -1;
    for (int i = 0; i < 8; i++) {
        int r = A.sess.StateRow(i);
        if (r >= 0 && r <= A.reached && r <= next && r > bestRow) { best = i; bestRow = r; }
    }
    return best;
}

// The slot to save row `row` in: the same row, an empty or stale slot, or else the automatic state whose
// neighbours are closest together (so the saved rows stay spread out).
int PickSlot(int row) {
    int empty = -1, stale = -1;
    for (int i = 0; i < 8; i++) {
        int r = A.sess.StateRow(i);
        if (r == row) return i;
        if (r < 0) { if (empty < 0) empty = i; }
        else if (r > A.reached) { if (stale < 0) stale = i; }
    }
    if (empty >= 0) return empty;
    if (stale >= 0) return stale;
    int best = -1, bestGap = 0x7FFFFFFF;
    for (int i = 0; i < 8; i++) {
        if (A.ssManual[i]) continue;
        int r = A.sess.StateRow(i), lo = 0, hi = r + std::max(A.ssEvery, 1000);
        for (int j = 0; j < 8; j++) {
            if (j == i) continue;
            int q = A.sess.StateRow(j);
            if (q >= 0 && q <= r) lo = std::max(lo, q);
            if (q > r) hi = std::min(hi, q);
        }
        if (hi - lo < bestGap) { bestGap = hi - lo; best = i; }
    }
    return best;
}

// Saves the frozen game's position. Not for use while a batch is running.
bool SaveStateAt(bool manual) {
    if (!StatesOn()) return false;
    int row = (int)A.sess.Row();
    if (row < 0 || row > A.reached) return false;
    int slot = PickSlot(row);
    bool keep = A.sess.StateRow(slot) == row && A.ssManual[slot];      // saving over the same row keeps a manual state manual
    if (slot < 0 || !A.sess.SaveState(slot)) return false;
    A.ssManual[slot] = manual || keep;
    return true;
}

// Puts the game back to the latest state at or before the cursor row and runs it forward from there.
bool RewindWithState(int row) {
    if (A.busy || !StatesOn()) return false;
    int slot = BestState(row + 1);
    if (slot < 0) return false;
    int r = A.sess.StateRow(slot);
    DWORD t0 = GetTickCount();
    if (!A.sess.LoadState(slot)) {
        A.sess.Close();
        SetMsg(L"Loading the savestate failed; the game is gone. Rewind again to relaunch.");
        return true;
    }
    SetMsg(L"Loaded the state at frame " + std::to_wstring(r) + L" (" + std::to_wstring(GetTickCount() - t0) + L" ms).");
    UpdateStatus();
    InvalidateRect(A.grid, nullptr, FALSE);
    if (r <= row) RunToCursor();
    return true;
}

void OnJobDone(RunResult* r) {
    if (A.thread) { WaitForSingleObject(A.thread, 2000); CloseHandle(A.thread); A.thread = nullptr; }
    A.busy = false;
    EnableUi();

    if (A.jobRecord && r->ok) {
        if (r->recorded.empty()) {
            SetMsg(L"No new frames recorded.");
        } else {
            Step st; st.cur_before = A.cursor;
            if (A.jobNew) {
                Notes none(r->recorded.size());
                DoSplice(st, 0, Size(), r->recorded, &none);
                A.path.clear();
                A.movie.prelude.clear();
            } else {
                size_t at = r->first;
                DoSplice(st, at, std::min(r->recorded.size(), (size_t)Size() - at), r->recorded);
            }
            int end = (int)(r->first + r->recorded.size());
            A.cursor = A.anchor = end - 1;
            Commit(st);
            A.reached = end;        // the live game sits at the end of the recording
            SetCursorRow(end - 1, false);
            SetMsg(L"Recorded " + std::to_wstring(r->recorded.size()) + L" frames.");
        }
    } else if (r->ok) {
        A.reached = (int)r->played;
        SetCursorRow((int)r->played - 1, false);
        if (A.jobHold) {
            A.sess = A.jobSess;
            A.jobSess = Session();
            SetMsg(L"Game frozen after frame " + std::to_wstring(r->played) + L". Press . to advance a frame, F11 to resume live.");
            if (A.useStates) {
                for (bool& m : A.ssManual) m = false;       // a new game has no states yet
                if (SaveStateAt(false)) SetMsg(L"Game frozen after frame " + std::to_wstring(r->played) + L"; state saved. Press . to advance a frame, F11 to resume live.");
            }
        } else {
            SetMsg(L"Reached frame " + std::to_wstring(r->played) + L". The game is left running.");
        }
    } else if (r->error == "Stopped.") {
        A.reached = (int)r->played;
        SetMsg(L"Stopped at frame " + std::to_wstring(r->played) + L".");
    } else {
        SetMsg(L"Failed.");
        MessageBoxW(A.wnd, W(r->error).c_str(), L"bscotm-tas", MB_ICONERROR);
    }
    delete r;
    UpdateScroll(); UpdateStatus();
    SetFocus(A.grid);
    if (A.pendingStep) {
        A.pendingStep = false;
        if (A.sess.Active()) FrameAdvance();
    }
    if (A.pendingRun) {
        A.pendingRun = false;
        if (A.sess.Active()) RunToCursor();
    }
}

// ---- frame advance ----------------------------------------------------------
// The frozen game is stopped before the movie row it would run next (sess.Row()).
// Stepping feeds it that row's inputs and freezes it again.
void FrameAdvance() {
    if (A.busy) return;
    if (!A.sess.Active() || !A.sess.Alive()) {
        A.sess.Close();
        SetMsg(L"No frozen game. Use Rewind to cursor (F6) first, then advance with '.'.");
        return;
    }
    int row = (int)A.sess.Row();
    if (row < 0) { SetMsg(L"The game is not frozen."); return; }
    if (row > A.reached) {
        // An edit before the game's position invalidated it: replay to the greenzone edge.
        A.pendingStep = true;
        StartJob(false, (uint32_t)A.reached, false, false, true);
        if (!A.busy) A.pendingStep = false;
        return;
    }
    if (row >= Size()) {
        Step st; st.cur_before = A.cursor;
        EnsureRows(st, row + 1);
        Commit(st);
    }
    if (!A.sess.Step(A.movie.frames[row])) {
        A.sess.Close();
        SetMsg(L"The game stopped responding to frame advance.");
        return;
    }
    A.reached = row + 1;
    SetCursorRow(row, false);
    SetMsg(L"Frame " + std::to_wstring(row + 1) + L" advanced.");
}

// Advances the frozen game until it has run through the cursor row (the same stop point as
// Rewind to cursor), feeding it the grid's inputs. Past the end of the movie it appends blank
// frames. The batch runs at the fast-forward speed and does not block the editor.
void RunToCursor() {
    if (A.busy) return;
    if (!A.sess.Active() || !A.sess.Alive()) {
        A.sess.Close();
        SetMsg(L"No frozen game. Use Rewind to cursor (F6) first, then run to a later row.");
        return;
    }
    int row = (int)A.sess.Row();
    if (row < 0) { SetMsg(L"The game is not frozen."); return; }
    if (row > A.reached) {      // an edit before the game's position invalidated it: replay first
        A.pendingRun = true;
        StartJob(false, (uint32_t)A.reached, false, false, true);
        if (!A.busy) A.pendingRun = false;
        return;
    }
    int target = A.cursor + 1;
    if (row >= target) {
        SetMsg(L"The game is already at frame " + std::to_wstring(row) + L", at or past the cursor. Rewind to cursor (F6) goes back.");
        return;
    }
    Step st; st.cur_before = A.cursor;
    EnsureRows(st, target);
    Commit(st);
    // With savestates the batch runs in pieces ending at multiples of ssEvery, with a state saved at each.
    int end = target;
    if (StatesOn() && A.ssEvery > 0) end = std::min(target, (row / A.ssEvery + 1) * A.ssEvery);
    Frames keys(A.movie.frames.begin() + row, A.movie.frames.begin() + end);
    if (!A.sess.BeginSteps(keys.data(), (uint32_t)keys.size(), SPEEDS[A.speed])) {
        A.sess.Close();
        SetMsg(L"Could not start running the game.");
        return;
    }
    A.chunkFrom = row;
    A.runFrom = row; A.runTarget = target;
    A.stepping = true;
    A.busy = true;
    EnableUi();
    SetTimer(A.wnd, 2, 20, nullptr);
    UpdateScroll(); UpdateStatus();
}

void FinishRun(int result, bool aborted) {
    KillTimer(A.wnd, 2);
    A.stepping = false;
    A.busy = false;
    EnableUi();
    if (result < 0) {
        A.sess.Close();
        SetMsg(L"The game stopped responding while running to the cursor.");
    } else {
        int row = (int)A.sess.Row();
        if (row >= 0) A.reached = std::max(A.reached, row);
        SetMsg((aborted ? L"Stopped. Game frozen after frame " : L"Game frozen after frame ") + std::to_wstring(row) + L".");
    }
    InvalidateRect(A.grid, nullptr, FALSE);
    UpdateStatus();
    SetFocus(A.grid);
}

// Go to a frame: if the frozen game has not reached it yet, just run it forward at the fast-forward
// speed (no restart); if it is behind us (or there is no frozen game), rewind (restart + fast-forward).
void JumpTo(int row) {
    if (A.busy) { SetMsg(L"Wait for the running job to finish (Stop = F9) before jumping."); return; }
    if (!Size()) return;
    row = std::max(0, std::min(row, Size() - 1));
    SetCursorRow(row, false);
    bool frozen = A.sess.Active() && A.sess.Alive();
    int next = frozen ? (int)A.sess.Row() : -1;         // the row the frozen game runs next
    if (frozen && next >= 0 && next <= row) { RunToCursor(); return; }
    if (frozen && next == row + 1) { SetMsg(L"The game is already at frame " + std::to_wstring(row + 1) + L"."); return; }
    if (RewindWithState(row)) return;
    StartJob(false, (uint32_t)(row + 1), false, false, true);
}

void PollRun() {
    int r = A.sess.PollSteps();
    if (r == 0) {
        SetMsg(L"Running to the cursor: frame " + std::to_wstring(A.chunkFrom + (int)A.sess.StepsDone()) + L" / " +
               std::to_wstring(A.runTarget) + L". Stop (F9) to cancel.");
        return;
    }
    if (r == 1 && StatesOn()) {
        int row = (int)A.sess.Row();
        A.reached = std::max(A.reached, row);
        if (row >= 0 && (row == A.runTarget || (A.ssEvery > 0 && row % A.ssEvery == 0))) SaveStateAt(false);
        if (row >= 0 && row < A.runTarget) {            // the next piece
            int end = std::min(A.runTarget, A.ssEvery > 0 ? (row / A.ssEvery + 1) * A.ssEvery : A.runTarget);
            Frames keys(A.movie.frames.begin() + row, A.movie.frames.begin() + end);
            if (A.sess.BeginSteps(keys.data(), (uint32_t)keys.size(), SPEEDS[A.speed])) { A.chunkFrom = row; return; }
            r = -1;
        }
    }
    FinishRun(r, false);
}

// Record from the frozen game without relaunching: the game runs live, the hook records
// the keyboard (only while the game window is focused), and Stop freezes it again.
void RecordHere() {
    if (A.busy) return;
    if (!A.sess.Active() || !A.sess.Alive()) {
        A.sess.Close();
        SetMsg(L"No frozen game. Use Rewind to cursor (F6) first.");
        return;
    }
    int row = (int)A.sess.Row();
    if (row < 0) { SetMsg(L"The game is not frozen."); return; }
    if (row > A.reached) {      // an earlier row was edited: resync, then ask again
        SetMsg(L"The game is ahead of your edits. Rewind to cursor (F6) first.");
        return;
    }
    if (!A.sess.StartRecording()) { SetMsg(L"Could not start recording."); return; }
    A.liveRec = true;
    A.liveRecRow = row;
    A.busy = true;
    EnableUi();
    SetTimer(A.wnd, 1, 100, nullptr);
    SetMsg(L"RECORDING - click the game window and play; press Stop (F9) here when done.");
}

void StopRecordHere() {
    if (!A.liveRec) return;
    KillTimer(A.wnd, 1);
    Frames rec;
    bool frozen = A.sess.StopRecording(rec);
    A.liveRec = false;
    A.busy = false;
    EnableUi();
    if (rec.empty()) {
        SetMsg(L"No frames recorded.");
    } else {
        Step st; st.cur_before = A.cursor;
        size_t at = (size_t)A.liveRecRow;
        EnsureRows(st, at);
        DoSplice(st, at, std::min(rec.size(), (size_t)Size() - at), rec);
        int end = (int)(at + rec.size());
        A.cursor = A.anchor = end - 1;
        Commit(st);
        A.reached = end;        // the game is frozen right after the recording
        SetCursorRow(end - 1, false);
        SetMsg(L"Recorded " + std::to_wstring(rec.size()) + L" frames." +
               (frozen ? L" The game is frozen; '.' advances, F12 records again." : L" The game stopped."));
    }
    if (!frozen) A.sess.Close();
    UpdateScroll(); UpdateStatus();
    SetFocus(A.grid);
}

void ResumeLive() {
    if (A.busy) return;
    if (!A.sess.Active()) { SetMsg(L"No frozen game."); return; }
    A.sess.Release();
    SetMsg(L"Game resumed with the live keyboard (frames are not recorded).");
}

// ---- main window ----------------------------------------------------------
const wchar_t* const BTN_TEXT[8] = {L"Record new", L"Play", L"Rewind to cursor", L"Record from cursor", L"Frame advance", L"Run to cursor", L"Record here", L"Stop"};
const int BTN_ID[8] = {IDM_RECORD, IDM_PLAY, IDM_REWIND, IDM_RECFROM, IDM_STEP, IDM_RUNTO, IDM_RECHERE, IDM_STOP};

void EnableUi() {
    for (int i = 0; i < 7; i++) EnableWindow(A.btn[i], !A.busy);
    EnableWindow(A.btn[7], A.busy);
    EnableWindow(A.cbBase, !A.busy);
    EnableWindow(A.cbPre, !A.busy);
}

void BuildMenu(HWND w) {
    HMENU bar = CreateMenu(), f = CreatePopupMenu(), e = CreatePopupMenu(), r = CreatePopupMenu();
    auto add = [](HMENU m, int id, const wchar_t* t) { AppendMenuW(m, MF_STRING, id, t); };
    add(f, IDM_NEW, L"&New\tCtrl+N");
    add(f, IDM_OPEN, L"&Open...\tCtrl+O");
    add(f, IDM_SAVE, L"&Save\tCtrl+S");
    add(f, IDM_SAVEAS, L"Save &As...\tCtrl+Shift+S");
    add(f, IDM_SAVEPRE, L"Save as &prelude...");
    AppendMenuW(f, MF_SEPARATOR, 0, nullptr);
    add(f, IDM_SETGAME, L"Set &game path...");
    add(f, IDM_BASESAVE, L"Save current game saves as &baseline...");
    AppendMenuW(f, MF_SEPARATOR, 0, nullptr);
    add(f, IDM_EXIT, L"E&xit");
    add(e, IDM_UNDO, L"&Undo\tCtrl+Z");
    add(e, IDM_REDO, L"&Redo\tCtrl+Y");
    AppendMenuW(e, MF_SEPARATOR, 0, nullptr);
    add(e, IDM_CUT, L"Cu&t\tCtrl+X");
    add(e, IDM_COPY, L"&Copy\tCtrl+C");
    add(e, IDM_PASTE, L"&Paste (overwrite)\tCtrl+V");
    add(e, IDM_PASTEINS, L"Paste &insert\tCtrl+Shift+V");
    add(e, IDM_SELALL, L"Select &all\tCtrl+A");
    AppendMenuW(e, MF_SEPARATOR, 0, nullptr);
    add(e, IDM_CLEAR, L"C&lear inputs\tDel");
    add(e, IDM_INSERT, L"&Insert frames\tIns");
    add(e, IDM_DELFRAMES, L"&Delete frames\tCtrl+Del");
    add(e, IDM_REPEAT, L"Repeat selection...\tCtrl+R");
    add(e, IDM_PATTERN, L"Fill selection with a &pattern...\tCtrl+Shift+R");
    AppendMenuW(e, MF_SEPARATOR, 0, nullptr);
    add(e, IDM_NOTE_EDIT, L"Add / edit frame &note...");
    add(e, IDM_NOTE_DEL, L"Remove notes in selection");
    add(e, IDM_NOTE_LIST, L"Notes &list...");
    add(e, IDM_NOTENEXT, L"Next note\tAlt+Down");
    add(e, IDM_NOTEPREV, L"Previous note\tAlt+Up");
    AppendMenuW(e, MF_SEPARATOR, 0, nullptr);
    add(e, IDM_FIND, L"&Find...\tCtrl+F");
    add(e, IDM_FINDNEXT, L"Find &next\tF3");
    add(e, IDM_FINDPREV, L"Find pre&vious\tShift+F3");
    AppendMenuW(e, MF_SEPARATOR, 0, nullptr);
    add(e, IDM_BM_EDIT, L"Add / rename &bookmark...\tCtrl+B");
    add(e, IDM_BM_DEL, L"Remove bookmark\tCtrl+Shift+B");
    add(e, IDM_BM_LIST, L"Bookmarks lis&t...");
    add(r, IDM_PLAY, L"&Play from start\tF5");
    add(r, IDM_REWIND, L"&Rewind to cursor\tF6");
    add(r, IDM_RECFROM, L"Record &from cursor\tF7");
    add(r, IDM_RECORD, L"Record &new movie\tF8");
    add(r, IDM_STEP, L"Frame &advance\t.");
    add(r, IDM_RUNTO, L"Run to &cursor (advance the frozen game)\tF4");
    add(r, IDM_GOTO, L"&Jump to frame...\tCtrl+G");
    add(r, IDM_BM_NEXT, L"&Next bookmark (jump)\tF2");
    add(r, IDM_BM_PREV, L"Pre&vious bookmark (jump)\tShift+F2");
    add(r, IDM_RECHERE, L"Record &here (live, from the frozen game)\tF12");
    add(r, IDM_RESUME, L"Resume &live (unfreeze)\tF11");
    add(r, IDM_STOP, L"&Stop\tF9");
    AppendMenuW(r, MF_SEPARATOR, 0, nullptr);
    add(r, IDM_RNGSEED, L"RNG see&d...");
    add(r, IDM_VERIFY, L"&Verify fast-forward...");
    AppendMenuW(r, MF_SEPARATOR, 0, nullptr);
    add(r, IDM_SAVESTATES, L"Use &savestates (applies at the next launch)");
    add(r, IDM_SAVESTATE_NOW, L"Save s&tate at the game's position\tShift+F5");
    add(r, IDM_SSINTERVAL, L"Savestate &interval (frames)...");
    A.speedMenu = CreatePopupMenu();
    add(A.speedMenu, IDM_SPEED0, L"Real time (1x)");
    add(A.speedMenu, IDM_SPEED1, L"2x");
    add(A.speedMenu, IDM_SPEED2, L"8x");
    add(A.speedMenu, IDM_SPEED3, L"Max (24x)");
    AppendMenuW(r, MF_POPUP, (UINT_PTR)A.speedMenu, L"Fast-forward &speed (rewind / record from cursor)");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)f, L"&File");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)e, L"&Edit");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)r, L"&Run");
    AppendMenuW(bar, MF_STRING, IDM_MEMORY, L"&Memory");
    AppendMenuW(bar, MF_STRING, IDM_RNGLOG, L"RNG &log");     // a plain menu-bar button: opens the window
    HMENU hp = CreatePopupMenu();
    add(hp, IDM_HELP_GUIDE, L"&Guide...\tF1");
    add(hp, IDM_HELP_START, L"&Getting started");
    add(hp, IDM_HELP_KEYS, L"&Keyboard shortcuts");
    add(hp, IDM_HELP_TROUBLE, L"&Troubleshooting");
    AppendMenuW(hp, MF_SEPARATOR, 0, nullptr);
    add(hp, IDM_HELP_ABOUT, L"&About");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)hp, L"&Help");
    SetMenu(w, bar);
}

void Layout() {
    RECT rc;
    GetClientRect(A.wnd, &rc);
    SendMessageW(A.status, WM_SIZE, 0, 0);
    RECT sr;
    GetWindowRect(A.status, &sr);
    int sh = sr.bottom - sr.top, tb = S(38), x = S(6), y = S(5), bh = S(28);
    const int bw[8] = {S(96), S(60), S(124), S(142), S(110), S(110), S(100), S(60)};
    for (int i = 0; i < 8; i++) { MoveWindow(A.btn[i], x, y, bw[i], bh, TRUE); x += bw[i] + S(4); }
    x += S(10);
    MoveWindow(A.lblBase, x, y + S(5), S(52), S(20), TRUE); x += S(54);
    MoveWindow(A.cbBase, x, y, S(130), S(200), TRUE); x += S(138);
    MoveWindow(A.lblPre, x, y + S(5), S(48), S(20), TRUE); x += S(50);
    MoveWindow(A.cbPre, x, y, S(150), S(200), TRUE);
    MoveWindow(A.grid, 0, tb, rc.right, std::max(0, (int)rc.bottom - tb - sh), TRUE);
    int parts[2] = {rc.right - S(380), -1};
    SendMessageW(A.status, SB_SETPARTS, 2, (LPARAM)parts);
}

LRESULT CALLBACK MainProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            A.wnd = w;
            HDC dc = GetDC(nullptr);
            A.dpi = GetDeviceCaps(dc, LOGPIXELSY);
            ReleaseDC(nullptr, dc);
            NONCLIENTMETRICSW nm{sizeof nm};
            SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof nm, &nm, 0);
            A.font = CreateFontIndirectW(&nm.lfMessageFont);
            nm.lfMessageFont.lfWeight = FW_BOLD;
            A.fontB = CreateFontIndirectW(&nm.lfMessageFont);

            BuildMenu(w);
            A.status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, w, nullptr, nullptr, nullptr);
            for (int i = 0; i < 8; i++) {
                A.btn[i] = CreateWindowExW(0, L"BUTTON", BTN_TEXT[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                           0, 0, 0, 0, w, (HMENU)(INT_PTR)BTN_ID[i], nullptr, nullptr);
                SendMessageW(A.btn[i], WM_SETFONT, (WPARAM)A.font, TRUE);
            }
            A.lblBase = CreateWindowExW(0, L"STATIC", L"Baseline:", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, w, nullptr, nullptr, nullptr);
            A.lblPre = CreateWindowExW(0, L"STATIC", L"Prelude:", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, w, nullptr, nullptr, nullptr);
            A.cbBase = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                       0, 0, 0, 0, w, (HMENU)IDC_BASE, nullptr, nullptr);
            A.cbPre = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                      0, 0, 0, 0, w, (HMENU)IDC_PRE, nullptr, nullptr);
            for (HWND c : {A.lblBase, A.lblPre, A.cbBase, A.cbPre, A.status}) SendMessageW(c, WM_SETFONT, (WPARAM)A.font, TRUE);
            A.grid = CreateWindowExW(WS_EX_CLIENTEDGE, L"TasGrid", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP,
                                     0, 0, 0, 0, w, nullptr, nullptr, nullptr);
            ResolvePaths();
            {
                std::wstring sp = IniGet(L"last", L"speed");
                A.speed = sp.empty() ? 3 : std::max(0, std::min(3, _wtoi(sp.c_str())));
                CheckMenuRadioItem(A.speedMenu, IDM_SPEED0, IDM_SPEED3, IDM_SPEED0 + A.speed, MF_BYCOMMAND);
                A.useStates = IniGet(L"last", L"savestates") != L"0";
                std::wstring ev = IniGet(L"last", L"ssevery");
                if (!ev.empty()) A.ssEvery = std::max(0, _wtoi(ev.c_str()));
            }
            RefreshCombos(IniGet(L"last", L"baseline"), IniGet(L"last", L"prelude"));
            EnableUi();
            Layout();
            UpdateTitle(); UpdateScroll(); UpdateStatus();
            SetMsg(Exists(A.exe) ? L"Ready." : L"Ready. Game path not set (File > Set game path).");
            return 0;
        }
        case WM_SIZE: if (A.status) Layout(); return 0;
        case WM_GETMINMAXINFO: ((MINMAXINFO*)lp)->ptMinTrackSize = {S(1240), S(300)}; return 0;
        case WM_SETFOCUS: SetFocus(A.grid); return 0;
        case WM_INITMENUPOPUP: {
            HMENU m = (HMENU)wp;
            auto en = [&](int id, bool on) { EnableMenuItem(m, id, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED)); };
            for (int id : {IDM_NEW, IDM_OPEN, IDM_CUT, IDM_PASTE, IDM_PASTEINS, IDM_CLEAR, IDM_INSERT, IDM_DELFRAMES,
                           IDM_PLAY, IDM_REWIND, IDM_RECFROM, IDM_RECORD, IDM_BASESAVE, IDM_STEP, IDM_RESUME, IDM_RECHERE,
                           IDM_NOTE_EDIT, IDM_RUNTO, IDM_RNGSEED, IDM_VERIFY, IDM_BM_EDIT, IDM_GOTO, IDM_BM_NEXT, IDM_BM_PREV, IDM_REPEAT, IDM_PATTERN, IDM_FIND, IDM_FINDNEXT, IDM_FINDPREV, IDM_NOTENEXT, IDM_NOTEPREV})
                en(id, !A.busy);
            CheckMenuItem(m, IDM_SAVESTATES, MF_BYCOMMAND | (A.useStates ? MF_CHECKED : MF_UNCHECKED));
            en(IDM_SAVESTATES, !A.busy);
            en(IDM_SSINTERVAL, !A.busy);
            en(IDM_SAVESTATE_NOW, !A.busy && StatesOn());
            en(IDM_NOTE_DEL, !A.busy && SelectionHasNote());
            en(IDM_BM_DEL, !A.busy && RowIsBm(A.cursor));
            en(IDM_UNDO, !A.busy && !A.undo.empty());
            en(IDM_REDO, !A.busy && !A.redo.empty());
            en(IDM_STOP, A.busy);
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDM_NEW: FileNew(); break;
                case IDM_OPEN: FileOpen(); break;
                case IDM_SAVE: Save(); break;
                case IDM_SAVEAS: SaveAs(); break;
                case IDM_SAVEPRE: SavePrelude(); break;
                case IDM_SETGAME: EnsureGamePath(true); break;
                case IDM_BASESAVE: BaselineSave(); break;
                case IDM_EXIT: SendMessageW(w, WM_CLOSE, 0, 0); break;
                case IDM_UNDO: Undo(); break;
                case IDM_REDO: Redo(); break;
                case IDM_CUT: EditCut(); break;
                case IDM_COPY: EditCopy(); break;
                case IDM_PASTE: EditPaste(false); break;
                case IDM_PASTEINS: EditPaste(true); break;
                case IDM_SELALL: SelectAll(); break;
                case IDM_CLEAR: EditClear(); break;
                case IDM_INSERT: EditInsert(); break;
                case IDM_DELFRAMES: EditDelete(); break;
                case IDM_REPEAT: EditRepeat(); break;
                case IDM_FIND: FindDialog(); break;
                case IDM_FINDNEXT: FindStep(true); break;
                case IDM_FINDPREV: FindStep(false); break;
                case IDM_NOTENEXT: NoteStep(true); break;
                case IDM_NOTEPREV: NoteStep(false); break;
                case IDM_PATTERN: EditPattern(); break;
                case IDM_NOTE_EDIT: EditNote(); break;
                case IDM_NOTE_DEL: RemoveNotesInSelection(); break;
                case IDM_NOTE_LIST: ShowNotesList(false); break;
                case IDM_BM_LIST: ShowNotesList(true); break;
                case IDM_BM_EDIT: EditBookmark(); break;
                case IDM_BM_DEL: RemoveBookmark(A.cursor); break;
                case IDM_BM_NEXT: NextBookmark(true); break;
                case IDM_BM_PREV: NextBookmark(false); break;
                case IDM_GOTO: GoToFrame(); break;
                case IDM_JUMPCUR: JumpTo(A.cursor); break;
                case IDM_RNGSEED: EditSeed(); break;
                case IDM_VERIFY: VerifyFastForward(); break;
                case IDM_MEMORY: ShowMemory(); break;
                case IDM_HELP_GUIDE: case IDM_HELP_START: ShowHelp(0); break;
                case IDM_HELP_KEYS: ShowHelp(8); break;
                case IDM_HELP_TROUBLE: ShowHelp(9); break;
                case IDM_HELP_ABOUT: ShowHelp(NUM_HELP_TOPICS - 1); break;
                case IDM_RNGLOG: ShowRngLog(); break;
                case IDM_PLAY: StartJob(false, (uint32_t)Size(), false, true); break;
                case IDM_REWIND:
                    if (Size() && RewindWithState(std::min(A.cursor, Size() - 1))) break;
                    StartJob(false, (uint32_t)std::min(A.cursor + 1, Size()), false, false, true);
                    break;
                case IDM_SAVESTATES:
                    A.useStates = !A.useStates;
                    IniSet(L"last", L"savestates", A.useStates ? L"1" : L"0");
                    SetMsg(A.useStates ? L"Savestates on: the game launched from now on can save and load states (Rewind and Jump use them)."
                                       : L"Savestates off for the next launch.");
                    break;
                case IDM_SSINTERVAL: {
                    if (A.busy) break;
                    std::wstring t = std::to_wstring(A.ssEvery);
                    if (!AskText(L"Savestate interval", L"Frames between automatic savestates while the game is advanced (0 = never; 100 - 20000):", t)) break;
                    wchar_t* end = nullptr;
                    long n = wcstol(t.c_str(), &end, 10);
                    if (t.empty() || *end || n < 0 || (n > 0 && n < 100) || n > 20000) {
                        MessageBoxW(A.wnd, L"Enter 0, or a number of frames from 100 to 20000.", L"Savestate interval", MB_ICONERROR);
                        break;
                    }
                    A.ssEvery = (int)n;
                    IniSet(L"last", L"ssevery", std::to_wstring(A.ssEvery));
                    SetMsg(n ? L"A savestate is made every " + std::to_wstring(n) + L" frames while the game is advanced. Shorter = faster rewinds, more memory."
                             : L"No automatic savestates (Shift+F5 still saves one by hand).");
                    break;
                }
                case IDM_SAVESTATE_NOW:
                    if (A.busy) break;
                    if (!StatesOn()) { SetMsg(A.useStates ? L"No frozen game that can save states. Rewind to cursor (F6) first." : L"Savestates are off (Run > Use savestates), then Rewind to cursor (F6)."); break; }
                    if (SaveStateAt(true)) SetMsg(L"State saved at frame " + std::to_wstring(A.sess.Row()) + L" (kept until the game is relaunched).");
                    else SetMsg(L"Could not save a state here.");
                    UpdateStatus();
                    break;
                case IDM_STEP: FrameAdvance(); break;
                case IDM_RUNTO: RunToCursor(); break;
                case IDM_RECHERE: RecordHere(); break;
                case IDM_RESUME: ResumeLive(); break;
                case IDM_RECFROM: StartJob(true, (uint32_t)std::min(A.cursor + 1, Size()), false); break;
                case IDM_RECORD:
                    if (!A.busy && Size() && MessageBoxW(w, L"Replace the current movie with a new recording?\n(You can undo this.)",
                                                         L"Record new", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) break;
                    StartJob(true, 0, true);
                    break;
                case IDM_SPEED0: case IDM_SPEED1: case IDM_SPEED2: case IDM_SPEED3:
                    A.speed = LOWORD(wp) - IDM_SPEED0;
                    CheckMenuRadioItem(A.speedMenu, IDM_SPEED0, IDM_SPEED3, LOWORD(wp), MF_BYCOMMAND);
                    IniSet(L"last", L"speed", std::to_wstring(A.speed));
                    break;
                case IDM_STOP:
                    if (A.liveRec) StopRecordHere();
                    else if (A.stepping) { A.sess.AbortSteps(); FinishRun(1, true); }
                    else if (A.busy) { A.stop = 1; SetMsg(L"Stopping..."); }
                    break;
                case IDC_BASE: case IDC_PRE:
                    if (HIWORD(wp) == CBN_SELCHANGE) {
                        IniSet(L"last", L"baseline", ComboSel(A.cbBase));
                        IniSet(L"last", L"prelude", ComboSel(A.cbPre));
                        A.reached = 0;
                        InvalidateRect(A.grid, nullptr, FALSE);
                    }
                    break;
            }
            if (HIWORD(wp) == BN_CLICKED && lp) SetFocus(A.grid);
            return 0;
        case WM_TIMER:
            if (wp == 2 && A.stepping) { PollRun(); return 0; }
            if (wp == 1 && A.liveRec) {
                if (!A.sess.Alive()) { StopRecordHere(); break; }
                SetMsg(L"RECORDING from frame " + std::to_wstring(A.liveRecRow + 1) + L" - " +
                       std::to_wstring(A.sess.RecCount()) + L" frames. Click the game window and play; press Stop (F9) here when done.");
            }
            return 0;
        case WM_JOB_PROGRESS: OnJobProgress((Phase)wp, (uint32_t)lp); return 0;
        case WM_JOB_DONE: OnJobDone((RunResult*)lp); return 0;
        case WM_VERIFY_PROGRESS: { std::wstring* t = (std::wstring*)lp; if (A.busy) SetMsg(*t); delete t; return 0; }
        case WM_VERIFY_DONE: OnVerifyDone((std::wstring*)lp); return 0;
        case WM_CLOSE:
            if (A.busy) {
                A.stop = 1;
                if (A.thread) WaitForSingleObject(A.thread, 5000);
            }
            if (ConfirmDiscard()) DestroyWindow(w);
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdline, int show) {
    INITCOMMONCONTROLSEX ic{sizeof ic, ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&ic);

    WNDCLASSEXW g{sizeof g};
    g.style = CS_DBLCLKS;
    g.lpfnWndProc = GridProc;
    g.hInstance = inst;
    g.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    g.lpszClassName = L"TasGrid";
    RegisterClassExW(&g);

    WNDCLASSEXW nd{sizeof nd};
    nd.lpfnWndProc = NoteDlgProc;
    nd.hInstance = inst;
    nd.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    nd.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    nd.lpszClassName = L"BscotmNote";
    RegisterClassExW(&nd);
    nd.lpfnWndProc = NotesProc;
    nd.lpszClassName = L"BscotmNotes";
    RegisterClassExW(&nd);
    nd.lpfnWndProc = MemProc;
    nd.lpszClassName = L"BscotmMem";
    RegisterClassExW(&nd);
    nd.lpfnWndProc = RngProc;
    nd.lpszClassName = L"BscotmRng";
    RegisterClassExW(&nd);
    nd.lpfnWndProc = GraphProc;
    nd.lpszClassName = L"BscotmGraph";
    RegisterClassExW(&nd);
    nd.lpfnWndProc = HelpProc;
    nd.lpszClassName = L"BscotmHelp";
    RegisterClassExW(&nd);

    WNDCLASSEXW c{sizeof c};
    c.lpfnWndProc = MainProc;
    c.hInstance = inst;
    c.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    c.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    c.lpszClassName = L"BscotmTas";
    RegisterClassExW(&c);

    HWND w = CreateWindowExW(0, L"BscotmTas", L"bscotm-tas", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1000, 720, nullptr, nullptr, inst, nullptr);
    if (!w) return 1;
    ShowWindow(w, show);
    if (cmdline && *cmdline) {         // bscotm_tas.exe movie.bscotm
        std::wstring a = cmdline;
        if (a.size() > 1 && a.front() == L'"') a = a.substr(1, a.rfind(L'"') - 1);
        LoadPath(a);
    }

    struct { BYTE f; WORD k, c; } keys[] = {
        {FCONTROL | FVIRTKEY, 'N', IDM_NEW},      {FCONTROL | FVIRTKEY, 'O', IDM_OPEN},
        {FCONTROL | FVIRTKEY, 'S', IDM_SAVE},     {FCONTROL | FSHIFT | FVIRTKEY, 'S', IDM_SAVEAS},
        {FCONTROL | FVIRTKEY, 'Z', IDM_UNDO},     {FCONTROL | FVIRTKEY, 'Y', IDM_REDO},
        {FCONTROL | FVIRTKEY, 'X', IDM_CUT},      {FCONTROL | FVIRTKEY, 'C', IDM_COPY},
        {FCONTROL | FVIRTKEY, 'V', IDM_PASTE},    {FCONTROL | FSHIFT | FVIRTKEY, 'V', IDM_PASTEINS},
        {FCONTROL | FVIRTKEY, 'A', IDM_SELALL},   {FVIRTKEY, VK_F5, IDM_PLAY},
        {FSHIFT | FVIRTKEY, VK_F5, IDM_SAVESTATE_NOW}, {FVIRTKEY, VK_F6, IDM_REWIND},            {FVIRTKEY, VK_F7, IDM_RECFROM},
        {FVIRTKEY, VK_F8, IDM_RECORD},            {FVIRTKEY, VK_F9, IDM_STOP},
        {FVIRTKEY, VK_OEM_PERIOD, IDM_STEP},      {FVIRTKEY, VK_F4, IDM_RUNTO},       {FVIRTKEY, VK_F1, IDM_HELP_GUIDE}, {FVIRTKEY, VK_F11, IDM_RESUME},       {FVIRTKEY, VK_F12, IDM_RECHERE},
        {FCONTROL | FVIRTKEY, 'B', IDM_BM_EDIT}, {FCONTROL | FSHIFT | FVIRTKEY, 'B', IDM_BM_DEL}, {FCONTROL | FVIRTKEY, 'G', IDM_GOTO}, {FCONTROL | FVIRTKEY, 'R', IDM_REPEAT}, {FCONTROL | FVIRTKEY, 'F', IDM_FIND}, {FVIRTKEY, VK_F3, IDM_FINDNEXT}, {FSHIFT | FVIRTKEY, VK_F3, IDM_FINDPREV}, {FALT | FVIRTKEY, VK_DOWN, IDM_NOTENEXT}, {FALT | FVIRTKEY, VK_UP, IDM_NOTEPREV}, {FCONTROL | FSHIFT | FVIRTKEY, 'R', IDM_PATTERN},
        {FVIRTKEY, VK_F2, IDM_BM_NEXT}, {FSHIFT | FVIRTKEY, VK_F2, IDM_BM_PREV},
    };
    ACCEL acc[sizeof keys / sizeof keys[0]];
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) acc[i] = ACCEL{keys[i].f, keys[i].k, keys[i].c};
    HACCEL ha = CreateAcceleratorTableW(acc, (int)(sizeof keys / sizeof keys[0]));

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!TranslateAcceleratorW(w, ha, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return (int)m.wParam;
}
