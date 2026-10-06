// bscotm-tas: TAStudio-style frame editor for Bloodstained: Curse of the Moon.
// Plain Win32 + GDI, no external dependencies.
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
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
    IDM_CLEAR, IDM_INSERT, IDM_DELFRAMES, IDM_NOTE_EDIT, IDM_NOTE_DEL, IDM_NOTE_LIST,
    IDM_RECORD, IDM_PLAY, IDM_REWIND, IDM_RECFROM, IDM_STOP,
    IDM_SPEED0, IDM_SPEED1, IDM_SPEED2, IDM_SPEED3, IDM_STEP, IDM_RESUME, IDM_RECHERE,
    IDC_BASE = 300, IDC_PRE,
};
const UINT WM_JOB_PROGRESS = WM_APP + 1;   // wParam = Phase, lParam = movie frame
const UINT WM_JOB_DONE     = WM_APP + 2;   // lParam = RunResult*

const COLORREF cBg = RGB(255, 255, 255), cHdr = RGB(236, 236, 240), cLine = RGB(222, 222, 228),
               cLine60 = RGB(140, 140, 155), cGreen = RGB(212, 240, 212), cGreenHead = RGB(140, 205, 140),
               cSel = RGB(204, 224, 255), cCursor = RGB(40, 90, 180), cPressed = RGB(52, 120, 200),
               cPhantom = RGB(170, 170, 175), cNote = RGB(255, 249, 212), cNoteText = RGB(90, 60, 0),
               cFlag = RGB(235, 170, 20);

typedef std::vector<std::string> Notes;   // UTF-8, parallel to the frames

// One undoable edit = a list of "replace range" operations (frames and their notes).
struct Splice { size_t pos; Frames oldv, newv; Notes oldn, newn; };
struct Step   { std::vector<Splice> parts; int cur_before = 0, cur_after = 0; };

struct App {
    HWND wnd = nullptr, grid = nullptr, status = nullptr, cbBase = nullptr, cbPre = nullptr;
    HWND lblBase = nullptr, lblPre = nullptr, btn[7] = {};
    HFONT font = nullptr, fontB = nullptr;
    HWND notesWnd = nullptr, notesList = nullptr;   // the Notes list window (when open)
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

void UpdateStatus() {
    int n = Size();
    wchar_t b[128];
    double t = (A.cursor + 1) / 60.0, tt = n / 60.0;
    swprintf(b, 128, L"Frame %d / %d   (%d:%05.2f / %d:%05.2f)   game at %d", n ? A.cursor + 1 : 0, n,
             (int)t / 60, t - 60 * ((int)t / 60), (int)tt / 60, tt - 60 * ((int)tt / 60), A.reached);
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

const std::string& NoteAt(int row) {
    static const std::string none;
    return row >= 0 && (size_t)row < A.movie.notes.size() ? A.movie.notes[row] : none;
}

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
        if (!NoteAt(r).empty()) return true;
    return false;
}

void RemoveNotesInSelection() {
    if (A.busy || !Size() || !SelectionHasNote()) return;
    int lo = SelLo(), hi = std::min(SelHi(), Size() - 1);
    Step st; st.cur_before = A.cursor;
    Notes none(hi - lo + 1);
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
    std::wstring t = W(NoteAt(row));
    if (!AskText(L"Frame note", L"Note for frame " + std::to_wstring(row + 1) + L" (leave empty to remove):", t)) return;
    for (wchar_t& c : t) if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
    SetNote(row, U8(t));
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
        Fill(m, gw, y, rc.right, y + rh, sel ? cSel : (note.empty() ? cBg : cNote));
        if (!note.empty()) {
            Fill(m, S(2), y + S(4), S(8), y + rh - S(4), cFlag);       // flag next to the frame number
            std::wstring wn = W(note);
            Text(m, wn.c_str(), RECT{gw + S(6), y, rc.right - S(4), y + rh}, DT_LEFT | DT_END_ELLIPSIS, cNoteText);
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
            AppendMenuW(pm, MF_STRING | (A.busy ? MF_GRAYED : 0), IDM_NOTE_EDIT, lbl.c_str());
            AppendMenuW(pm, MF_STRING | (A.busy || !hasNote ? MF_GRAYED : 0), IDM_NOTE_DEL, L"Remove note(s) in selection");
            AppendMenuW(pm, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(pm, MF_STRING, IDM_NOTE_LIST, L"Notes list...");
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
        if (A.movie.notes[r].empty()) continue;
        wchar_t b[16];
        swprintf(b, 16, L"%d", r + 1);
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = n++;
        it.pszText = b;
        it.lParam = r;
        int i = (int)SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
        std::wstring w = W(A.movie.notes[r]);
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
                if (row >= 0) SetCursorRow(row, false);
            } else if (nh->code == LVN_KEYDOWN && ((NMLVKEYDOWN*)lp)->wVKey == VK_DELETE) {
                int row = NotesSelectedRow();
                if (row >= 0) SetNote(row, "");
            }
            return 0;
        }
        case WM_CLOSE: DestroyWindow(h); return 0;
        case WM_DESTROY: A.notesWnd = A.notesList = nullptr; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void ShowNotesList() {
    if (A.notesWnd) { SetForegroundWindow(A.notesWnd); return; }
    RECT pr;
    GetWindowRect(A.wnd, &pr);
    A.notesWnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"BscotmNotes", L"Frame notes",
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

const uint32_t SPEEDS[4] = {1000, 4000, 16000, 50000};

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
            delete j;
            return;
        }
        p.prelude = pm.frames;
    }
    if (!fresh) p.movie = A.movie.frames;
    p.target = target;
    p.record = record;
    p.speed_milli = realtime ? 1000 : SPEEDS[A.speed];
    // The virtual clock is always on (identical to real time at 1x) so a frozen game
    // can be thawed without a burst of catch-up frames.
    p.speed_mask = p.speed_milli > 1000 ? SPEED_ALL : (SPEED_ALL & ~SPEED_NOVSYNC);
    p.hold = hold;

    A.jobRecord = record; A.jobNew = fresh; A.jobTarget = target; A.jobHold = hold;
    A.busy = true;
    A.stop = 0;
    EnableUi();
    SetMsg(L"Launching the game...");
    A.thread = CreateThread(nullptr, 0, JobThread, j, 0, nullptr);
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
void StopRecordHere();

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
const wchar_t* const BTN_TEXT[7] = {L"Record new", L"Play", L"Rewind to cursor", L"Record from cursor", L"Frame advance", L"Record here", L"Stop"};
const int BTN_ID[7] = {IDM_RECORD, IDM_PLAY, IDM_REWIND, IDM_RECFROM, IDM_STEP, IDM_RECHERE, IDM_STOP};

void EnableUi() {
    for (int i = 0; i < 6; i++) EnableWindow(A.btn[i], !A.busy);
    EnableWindow(A.btn[6], A.busy);
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
    AppendMenuW(e, MF_SEPARATOR, 0, nullptr);
    add(e, IDM_NOTE_EDIT, L"Add / edit frame &note...");
    add(e, IDM_NOTE_DEL, L"Remove notes in selection");
    add(e, IDM_NOTE_LIST, L"Notes &list...");
    add(r, IDM_PLAY, L"&Play from start\tF5");
    add(r, IDM_REWIND, L"&Rewind to cursor\tF6");
    add(r, IDM_RECFROM, L"Record &from cursor\tF7");
    add(r, IDM_RECORD, L"Record &new movie\tF8");
    add(r, IDM_STEP, L"Frame &advance\t.");
    add(r, IDM_RECHERE, L"Record &here (live, from the frozen game)\tF12");
    add(r, IDM_RESUME, L"Resume &live (unfreeze)\tF11");
    add(r, IDM_STOP, L"&Stop\tF9");
    AppendMenuW(r, MF_SEPARATOR, 0, nullptr);
    A.speedMenu = CreatePopupMenu();
    add(A.speedMenu, IDM_SPEED0, L"Real time (1x)");
    add(A.speedMenu, IDM_SPEED1, L"4x");
    add(A.speedMenu, IDM_SPEED2, L"16x");
    add(A.speedMenu, IDM_SPEED3, L"Max (50x)");
    AppendMenuW(r, MF_POPUP, (UINT_PTR)A.speedMenu, L"Fast-forward &speed (rewind / record from cursor)");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)f, L"&File");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)e, L"&Edit");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)r, L"&Run");
    SetMenu(w, bar);
}

void Layout() {
    RECT rc;
    GetClientRect(A.wnd, &rc);
    SendMessageW(A.status, WM_SIZE, 0, 0);
    RECT sr;
    GetWindowRect(A.status, &sr);
    int sh = sr.bottom - sr.top, tb = S(38), x = S(6), y = S(5), bh = S(28);
    const int bw[7] = {S(96), S(60), S(124), S(142), S(110), S(100), S(60)};
    for (int i = 0; i < 7; i++) { MoveWindow(A.btn[i], x, y, bw[i], bh, TRUE); x += bw[i] + S(4); }
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
            for (int i = 0; i < 7; i++) {
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
            }
            RefreshCombos(IniGet(L"last", L"baseline"), IniGet(L"last", L"prelude"));
            EnableUi();
            Layout();
            UpdateTitle(); UpdateScroll(); UpdateStatus();
            SetMsg(Exists(A.exe) ? L"Ready." : L"Ready. Game path not set (File > Set game path).");
            return 0;
        }
        case WM_SIZE: if (A.status) Layout(); return 0;
        case WM_GETMINMAXINFO: ((MINMAXINFO*)lp)->ptMinTrackSize = {S(1110), S(300)}; return 0;
        case WM_SETFOCUS: SetFocus(A.grid); return 0;
        case WM_INITMENUPOPUP: {
            HMENU m = (HMENU)wp;
            auto en = [&](int id, bool on) { EnableMenuItem(m, id, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED)); };
            for (int id : {IDM_NEW, IDM_OPEN, IDM_CUT, IDM_PASTE, IDM_PASTEINS, IDM_CLEAR, IDM_INSERT, IDM_DELFRAMES,
                           IDM_PLAY, IDM_REWIND, IDM_RECFROM, IDM_RECORD, IDM_BASESAVE, IDM_STEP, IDM_RESUME, IDM_RECHERE,
                           IDM_NOTE_EDIT})
                en(id, !A.busy);
            en(IDM_NOTE_DEL, !A.busy && SelectionHasNote());
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
                case IDM_NOTE_EDIT: EditNote(); break;
                case IDM_NOTE_DEL: RemoveNotesInSelection(); break;
                case IDM_NOTE_LIST: ShowNotesList(); break;
                case IDM_PLAY: StartJob(false, (uint32_t)Size(), false, true); break;
                case IDM_REWIND: StartJob(false, (uint32_t)std::min(A.cursor + 1, Size()), false, false, true); break;
                case IDM_STEP: FrameAdvance(); break;
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
            if (wp == 1 && A.liveRec) {
                if (!A.sess.Alive()) { StopRecordHere(); break; }
                SetMsg(L"RECORDING from frame " + std::to_wstring(A.liveRecRow + 1) + L" - " +
                       std::to_wstring(A.sess.RecCount()) + L" frames. Click the game window and play; press Stop (F9) here when done.");
            }
            return 0;
        case WM_JOB_PROGRESS: OnJobProgress((Phase)wp, (uint32_t)lp); return 0;
        case WM_JOB_DONE: OnJobDone((RunResult*)lp); return 0;
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
        {FVIRTKEY, VK_F6, IDM_REWIND},            {FVIRTKEY, VK_F7, IDM_RECFROM},
        {FVIRTKEY, VK_F8, IDM_RECORD},            {FVIRTKEY, VK_F9, IDM_STOP},
        {FVIRTKEY, VK_OEM_PERIOD, IDM_STEP},      {FVIRTKEY, VK_F11, IDM_RESUME},       {FVIRTKEY, VK_F12, IDM_RECHERE},
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
