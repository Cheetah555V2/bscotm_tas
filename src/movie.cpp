#include "movie.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

// ---- minimal JSON reader ---------------------------------------------------
struct J {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    double n = 0;
    std::string s;
    std::vector<std::string> keys;   // Obj
    std::vector<J> v;                // Arr items / Obj values
    const J* get(const char* k) const {
        for (size_t i = 0; i < keys.size(); i++) if (keys[i] == k) return &v[i];
        return nullptr;
    }
};

struct Parser {
    const char *p, *e;
    bool ok = true;
    void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
    bool lit(const char* w) {
        size_t n = strlen(w);
        if ((size_t)(e - p) < n || memcmp(p, w, n)) return ok = false;
        p += n; return true;
    }
    void utf8(std::string& o, unsigned c) {
        if (c < 0x80) o += (char)c;
        else if (c < 0x800) { o += (char)(0xC0 | c >> 6); o += (char)(0x80 | (c & 63)); }
        else { o += (char)(0xE0 | c >> 12); o += (char)(0x80 | ((c >> 6) & 63)); o += (char)(0x80 | (c & 63)); }
    }
    bool str(std::string& o) {
        if (p >= e || *p != '"') return ok = false;
        for (p++; p < e && *p != '"'; p++) {
            if (*p != '\\') { o += *p; continue; }
            if (++p >= e) break;
            switch (*p) {
                case 'n': o += '\n'; break;
                case 't': o += '\t'; break;
                case 'r': o += '\r'; break;
                case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case 'u': {
                    if (e - p < 5) return ok = false;
                    char h[5] = {p[1], p[2], p[3], p[4], 0};
                    utf8(o, (unsigned)strtoul(h, nullptr, 16));
                    p += 4;
                    break;
                }
                default: o += *p;
            }
        }
        if (p >= e) return ok = false;
        p++;
        return true;
    }
    bool val(J& j) {
        ws();
        if (p >= e) return ok = false;
        switch (*p) {
            case '{':
                j.t = J::Obj; p++; ws();
                if (p < e && *p == '}') { p++; return true; }
                while (ok) {
                    ws();
                    std::string k;
                    if (!str(k)) return false;
                    ws();
                    if (p >= e || *p++ != ':') return ok = false;
                    j.keys.push_back(std::move(k));
                    j.v.emplace_back();
                    if (!val(j.v.back())) return false;
                    ws();
                    if (p < e && *p == ',') { p++; continue; }
                    if (p < e && *p == '}') { p++; return true; }
                    return ok = false;
                }
                return false;
            case '[':
                j.t = J::Arr; p++; ws();
                if (p < e && *p == ']') { p++; return true; }
                while (ok) {
                    j.v.emplace_back();
                    if (!val(j.v.back())) return false;
                    ws();
                    if (p < e && *p == ',') { p++; continue; }
                    if (p < e && *p == ']') { p++; return true; }
                    return ok = false;
                }
                return false;
            case '"': j.t = J::Str; return str(j.s);
            case 't': j.t = J::Bool; j.n = 1; return lit("true");
            case 'f': j.t = J::Bool; return lit("false");
            case 'n': return lit("null");
            default: {
                char* end;
                j.t = J::Num;
                j.n = strtod(p, &end);
                if (end == p) return ok = false;
                p = end;
                return true;
            }
        }
    }
};

bool Truthy(const J& j) { return (j.t == J::Num || j.t == J::Bool) ? j.n != 0 : j.t != J::Null; }

// A frame key is either a name ("SPACE", older files) or a decimal VK ("32").
int BitFromKey(const std::string& k) {
    for (int i = 0; i < NUM_KEYS; i++) if (k == KEYS[i].name) return i;
    char* end;
    long vk = strtol(k.c_str(), &end, 10);
    if (end != k.c_str() && !*end)
        for (int i = 0; i < NUM_KEYS; i++) if (KEYS[i].vk == vk) return i;
    return -1;
}

// Sparse deltas ("same as previous, except...") -> dense masks.
void Expand(const J& arr, Frames& out) {
    KeyMask cur = 0;
    out.clear();
    out.reserve(arr.v.size());
    for (const J& f : arr.v) {
        for (size_t i = 0; i < f.keys.size(); i++) {
            int b = BitFromKey(f.keys[i]);
            if (b < 0) continue;
            if (Truthy(f.v[i])) cur |= KeyBit(b);
            else cur &= ~KeyBit(b);
        }
        out.push_back(cur);
    }
}

void Esc(FILE* f, const std::string& s) {
    fputc('"', f);
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

void WriteFrames(FILE* f, const Frames& fr) {
    fputc('[', f);
    KeyMask prev = 0;
    for (size_t i = 0; i < fr.size(); i++) {
        if (i) fputc(',', f);
        fputc('{', f);
        bool first = true;
        for (int b = 0; b < NUM_KEYS; b++) {
            int was = (prev >> b) & 1, now = (fr[i] >> b) & 1;
            if (was == now) continue;
            if (IsPadKey(b)) fprintf(f, "%s\"%s\":%d", first ? "" : ",", KEYS[b].name, now);     // controller keys have no VK: by name
            else fprintf(f, "%s\"%d\":%d", first ? "" : ",", KEYS[b].vk, now);
            first = false;
        }
        fputc('}', f);
        prev = fr[i];
    }
    fputc(']', f);
}

// Notes: sparse [{"frame": 12, "text": "...", "bookmark": true}], frame is 1-based; a bookmark's text may be empty.
void ReadNotes(const J* nt, size_t n, std::vector<std::string>& notes) {
    notes.assign(n, std::string());
    if (!nt || nt->t != J::Arr) return;
    for (const J& e : nt->v) {
        const J* fn = e.get("frame");
        const J* tx = e.get("text");
        if (!fn || !tx || fn->t != J::Num || tx->t != J::Str) continue;
        long idx = (long)fn->n - 1;
        const J* bm = e.get("bookmark");
        if (idx >= 0 && (size_t)idx < notes.size()) notes[idx] = (bm && Truthy(*bm) ? std::string(1, kBookmarkFlag) : std::string()) + tx->s;
    }
}

void WriteNotes(FILE* f, const std::vector<std::string>& notes, size_t n) {
    fputc('[', f);
    bool first = true;
    for (size_t i = 0; i < notes.size() && i < n; i++) {
        if (notes[i].empty()) continue;
        bool bm = notes[i][0] == kBookmarkFlag;
        fprintf(f, "%s{\"frame\":%u,\"text\":", first ? "" : ",", (unsigned)i + 1);
        Esc(f, bm ? notes[i].substr(1) : notes[i]);
        if (bm) fputs(",\"bookmark\":true", f);
        fputc('}', f);
        first = false;
    }
    fputc(']', f);
}

}  // namespace

bool Movie::Load(const std::wstring& path, std::string& err) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) { err = "cannot open file"; return false; }
    std::string buf;
    static char tmp[1 << 16];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) buf.append(tmp, n);
    fclose(f);

    Parser ps{buf.data(), buf.data() + buf.size()};
    J root;
    if (!ps.val(root) || root.t != J::Obj) { err = "not a valid JSON movie"; return false; }
    const J* fmt = root.get("format");
    if (!fmt || fmt->s != "bscotm-tas") { err = "not a bscotm-tas file"; return false; }
    const J* fr = root.get("frames");
    if (!fr || fr->t != J::Arr) { err = "missing frames"; return false; }

    auto str = [&](const char* k) {
        const J* j = root.get(k);
        return j && j->t == J::Str ? j->s : std::string();
    };
    author = str("author");
    created = str("created_utc");
    prelude_id = str("prelude_id");
    prelude_desc = str("prelude_description");
    has_seed = false; seed = 0;
    const J* sd = root.get("rng_seed");
    if (sd && sd->t == J::Num && sd->n >= 0 && sd->n <= 4294967295.0) { has_seed = true; seed = (uint32_t)sd->n; }
    const J* ct = root.get("controller");     // the game sees a (virtual) controller: see Movie::controller
    controller = ct && Truthy(*ct);
    Expand(*fr, frames);
    ReadNotes(root.get("notes"), frames.size(), notes);
    prelude.clear();
    const J* pf = root.get("prelude_frames");
    if (pf && pf->t == J::Arr) Expand(*pf, prelude);
    branches.clear();
    const J* br = root.get("branches");      // optional: [{"name", "saved", "frame", "rng_seed", "controller", "frames", "notes"}]
    if (br && br->t == J::Arr)
        for (const J& e : br->v) {
            const J* bf = e.get("frames");
            if (e.t != J::Obj || !bf || bf->t != J::Arr) continue;
            Branch b;
            const J* s;
            if ((s = e.get("name")) && s->t == J::Str) b.name = s->s;
            if ((s = e.get("saved")) && s->t == J::Str) b.saved = s->s;
            if ((s = e.get("frame")) && s->t == J::Num && s->n >= 0) b.frame = (uint32_t)s->n;
            if ((s = e.get("rng_seed")) && s->t == J::Num && s->n >= 0 && s->n <= 4294967295.0) { b.has_seed = true; b.seed = (uint32_t)s->n; }
            b.controller = (s = e.get("controller")) && Truthy(*s);
            Expand(*bf, b.frames);
            ReadNotes(e.get("notes"), b.frames.size(), b.notes);
            branches.push_back(std::move(b));
        }
    return true;
}

bool Movie::Save(const std::wstring& path, std::string& err) const {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) { err = "cannot write file"; return false; }
    KeyMask used = 0;
    for (KeyMask m : frames) used |= m;

    fputs("{\"format\":\"bscotm-tas\",\"format_version\":1,"
          "\"game\":\"Bloodstained: Curse of the Moon\",\"game_version\":\"1.1.2\","
          "\"platform\":\"steam-windows-x86\",\"author\":", f);
    Esc(f, author);
    fputs(",\"created_utc\":", f);
    Esc(f, created);
    if (has_seed) fprintf(f, ",\"rng_seed\":%u", (unsigned)seed); else fputs(",\"rng_seed\":null", f);
    if (controller) fputs(",\"controller\":true", f);       // only when on, so keyboard movies are written as before
    fputs(",\"anchors\":[],\"prelude_id\":", f);
    if (prelude_id.empty()) fputs("null", f); else Esc(f, prelude_id);
    fputs(",\"prelude_description\":", f);
    Esc(f, prelude_desc);
    fputs(",\"prelude_frames\":", f);
    if (prelude.empty()) fputs("null", f); else WriteFrames(f, prelude);
    fputs(",\"keys_used\":[", f);
    bool first = true;
    for (int b = 0; b < NUM_KB_KEYS; b++)          // keyboard keys only (they have a VK)
        if (used >> b & 1) { fprintf(f, "%s%d", first ? "" : ",", KEYS[b].vk); first = false; }
    fprintf(f, "],\"total_frames\":%u,\"frames\":", (unsigned)frames.size());
    WriteFrames(f, frames);
    fputs(",\"notes\":", f);
    WriteNotes(f, notes, frames.size());
    if (!branches.empty()) {
        fputs(",\"branches\":[", f);
        for (size_t i = 0; i < branches.size(); i++) {
            const Branch& b = branches[i];
            fputs(i ? ",{\"name\":" : "{\"name\":", f);
            Esc(f, b.name);
            fputs(",\"saved\":", f);
            Esc(f, b.saved);
            fprintf(f, ",\"frame\":%u", (unsigned)b.frame);
            if (b.has_seed) fprintf(f, ",\"rng_seed\":%u", (unsigned)b.seed); else fputs(",\"rng_seed\":null", f);
            if (b.controller) fputs(",\"controller\":true", f);
            fputs(",\"frames\":", f);
            WriteFrames(f, b.frames);
            fputs(",\"notes\":", f);
            WriteNotes(f, b.notes, b.frames.size());
            fputc('}', f);
        }
        fputc(']', f);
    }
    fputc('}', f);
    if (fclose(f) != 0) { err = "write failed"; return false; }
    return true;
}
