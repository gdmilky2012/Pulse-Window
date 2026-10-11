// Pulse Window - pembaca macro (hanya STL, tidak bergantung pada Geode).
// Hanya membaca daftar input (frame, tombol, player, down/up) untuk dianalisis.
// Tidak memutar ulang macro.
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace pmacro {

struct Input {
    uint64_t frame = 0;
    uint8_t  button = 1;   // 1 = jump, 2 = kiri, 3 = kanan
    bool     p2 = false;
    bool     down = false;
};

struct Macro {
    std::string format;
    double fps = 0.0;
    bool fpsChanged = false;   // macro mengubah TPS di tengah jalan (tidak dikonversi)
    std::vector<Input> inputs;
};

struct Result {
    bool ok = false;
    std::string error;
    Macro macro;
};

struct Stats {
    std::map<int, int> hist;   // panjang hold (frame) -> jumlah
    int presses = 0;
    int holds = 0;
    int longest = 0;
    double fps = 0.0;
};

namespace detail {

struct Fail : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ---------- pembaca byte ----------
struct Reader {
    const uint8_t* d;
    size_t n;
    size_t p = 0;
    Reader(const uint8_t* data, size_t size) : d(data), n(size) {}

    size_t remaining() const { return n - p; }
    void need(size_t k) const {
        if (k > n - p) throw Fail("file terpotong / tidak lengkap");
    }
    uint8_t u8() { need(1); return d[p++]; }
    void skip(size_t k) { need(k); p += k; }
    uint32_t u32le() {
        need(4);
        uint32_t v = d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | (uint32_t(d[p + 3]) << 24);
        p += 4;
        return v;
    }
    uint64_t u64le() {
        need(8);
        uint64_t v = 0;
        for (int i = 7; i >= 0; i--) v = (v << 8) | d[p + i];
        p += 8;
        return v;
    }
    float f32le() { uint32_t b = u32le(); float f; std::memcpy(&f, &b, 4); return f; }
    double f64le() { uint64_t b = u64le(); double f; std::memcpy(&f, &b, 8); return f; }
    float f32be() {
        need(4);
        uint32_t b = (uint32_t(d[p]) << 24) | (d[p + 1] << 16) | (d[p + 2] << 8) | d[p + 3];
        p += 4;
        float f; std::memcpy(&f, &b, 4); return f;
    }
    double f64be() {
        need(8);
        uint64_t b = 0;
        for (int i = 0; i < 8; i++) b = (b << 8) | d[p + i];
        p += 8;
        double f; std::memcpy(&f, &b, 8); return f;
    }
    // LEB128 (tepi lanjut = bit 0x80), maks 9 byte seperti implementasi GDR
    uint64_t varint() {
        uint64_t r = 0;
        for (int i = 0; i < 9; i++) {
            uint8_t b = u8();
            r |= uint64_t(b & 0x7F) << (i * 7);
            if (!(b & 0x80)) return r;
        }
        throw Fail("varint terlalu panjang");
    }
    std::string str() {
        uint64_t len = varint();
        if (len > 0xFFFF) throw Fail("string terlalu panjang");
        need(size_t(len));
        std::string s(reinterpret_cast<const char*>(d + p), size_t(len));
        p += size_t(len);
        return s;
    }
};

// ---------- nilai generik untuk JSON / MessagePack ----------
struct Value {
    enum Type { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Value> a;
    std::vector<std::pair<std::string, Value>> o;

    const Value* get(const char* key) const {
        if (t != Obj) return nullptr;
        for (auto const& kv : o) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    double num(double def = 0) const { return t == Num ? n : (t == Bool ? (b ? 1 : 0) : def); }
    bool boolean(bool def = false) const { return t == Bool ? b : (t == Num ? n != 0 : def); }
};

struct JsonParser {
    const char* s;
    size_t n;
    size_t i = 0;
    int depth = 0;
    JsonParser(const uint8_t* d, size_t size) : s(reinterpret_cast<const char*>(d)), n(size) {}

    void ws() { while (i < n && std::isspace(static_cast<unsigned char>(s[i]))) i++; }
    [[noreturn]] void bad() { throw Fail("JSON tidak valid"); }

    std::string str() {
        if (i >= n || s[i] != '"') bad();
        i++;
        std::string out;
        while (i < n && s[i] != '"') {
            char c = s[i++];
            if (c == '\\') {
                if (i >= n) bad();
                char e = s[i++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': if (i + 4 > n) bad(); i += 4; out += '?'; break;
                    default: out += e;
                }
            } else out += c;
        }
        if (i >= n) bad();
        i++;
        return out;
    }

    Value value() {
        if (++depth > 64) bad();
        ws();
        if (i >= n) bad();
        Value v;
        char c = s[i];
        if (c == '{') {
            i++; v.t = Value::Obj; ws();
            if (i < n && s[i] == '}') { i++; depth--; return v; }
            for (;;) {
                ws();
                std::string k = str();
                ws();
                if (i >= n || s[i++] != ':') bad();
                v.o.emplace_back(std::move(k), value());
                ws();
                if (i >= n) bad();
                if (s[i] == ',') { i++; continue; }
                if (s[i] == '}') { i++; break; }
                bad();
            }
        } else if (c == '[') {
            i++; v.t = Value::Arr; ws();
            if (i < n && s[i] == ']') { i++; depth--; return v; }
            for (;;) {
                v.a.push_back(value());
                ws();
                if (i >= n) bad();
                if (s[i] == ',') { i++; continue; }
                if (s[i] == ']') { i++; break; }
                bad();
            }
        } else if (c == '"') {
            v.t = Value::Str; v.s = str();
        } else if (!std::strncmp(s + i, "true", std::min<size_t>(4, n - i)) && n - i >= 4) {
            v.t = Value::Bool; v.b = true; i += 4;
        } else if (n - i >= 5 && !std::strncmp(s + i, "false", 5)) {
            v.t = Value::Bool; v.b = false; i += 5;
        } else if (n - i >= 4 && !std::strncmp(s + i, "null", 4)) {
            i += 4;
        } else {
            char* end = nullptr;
            std::string tmp(s + i, std::min<size_t>(n - i, 40));
            double d = std::strtod(tmp.c_str(), &end);
            if (end == tmp.c_str()) bad();
            i += size_t(end - tmp.c_str());
            v.t = Value::Num; v.n = d;
        }
        depth--;
        return v;
    }
};

struct MsgPack {
    Reader r;
    int depth = 0;
    MsgPack(const uint8_t* d, size_t n) : r(d, n) {}

    uint64_t be(int bytes) {
        r.need(size_t(bytes));
        uint64_t v = 0;
        for (int k = 0; k < bytes; k++) v = (v << 8) | r.d[r.p++];
        return v;
    }
    Value str(size_t len) {
        r.need(len);
        Value v; v.t = Value::Str;
        v.s.assign(reinterpret_cast<const char*>(r.d + r.p), len);
        r.p += len;
        return v;
    }
    Value arr(size_t cnt) {
        if (cnt > r.remaining()) throw Fail("MessagePack tidak valid");
        Value v; v.t = Value::Arr;
        for (size_t k = 0; k < cnt; k++) v.a.push_back(value());
        return v;
    }
    Value map(size_t cnt) {
        if (cnt > r.remaining()) throw Fail("MessagePack tidak valid");
        Value v; v.t = Value::Obj;
        for (size_t k = 0; k < cnt; k++) {
            Value key = value();
            Value val = value();
            v.o.emplace_back(key.t == Value::Str ? key.s : std::string(), std::move(val));
        }
        return v;
    }
    Value num(double d) { Value v; v.t = Value::Num; v.n = d; return v; }

    Value value() {
        if (++depth > 64) throw Fail("MessagePack terlalu dalam");
        uint8_t c = r.u8();
        Value v;
        if (c <= 0x7f) v = num(c);
        else if (c >= 0xe0) v = num(int8_t(c));
        else if ((c & 0xf0) == 0x80) v = map(c & 0x0f);
        else if ((c & 0xf0) == 0x90) v = arr(c & 0x0f);
        else if ((c & 0xe0) == 0xa0) v = str(c & 0x1f);
        else switch (c) {
            case 0xc0: break;
            case 0xc2: v.t = Value::Bool; v.b = false; break;
            case 0xc3: v.t = Value::Bool; v.b = true; break;
            case 0xc4: r.skip(size_t(be(1))); break;
            case 0xc5: r.skip(size_t(be(2))); break;
            case 0xc6: r.skip(size_t(be(4))); break;
            case 0xca: v = num(r.f32be()); break;
            case 0xcb: v = num(r.f64be()); break;
            case 0xcc: v = num(double(be(1))); break;
            case 0xcd: v = num(double(be(2))); break;
            case 0xce: v = num(double(be(4))); break;
            case 0xcf: v = num(double(be(8))); break;
            case 0xd0: v = num(double(int8_t(be(1)))); break;
            case 0xd1: v = num(double(int16_t(be(2)))); break;
            case 0xd2: v = num(double(int32_t(be(4)))); break;
            case 0xd3: v = num(double(int64_t(be(8)))); break;
            case 0xd9: v = str(size_t(be(1))); break;
            case 0xda: v = str(size_t(be(2))); break;
            case 0xdb: v = str(size_t(be(4))); break;
            case 0xdc: v = arr(size_t(be(2))); break;
            case 0xdd: v = arr(size_t(be(4))); break;
            case 0xde: v = map(size_t(be(2))); break;
            case 0xdf: v = map(size_t(be(4))); break;
            default: throw Fail("MessagePack: tipe tidak didukung");
        }
        depth--;
        return v;
    }
};

inline bool startsWith(std::vector<uint8_t> const& d, const char* magic) {
    size_t m = std::strlen(magic);
    return d.size() >= m && std::memcmp(d.data(), magic, m) == 0;
}

inline std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline bool endsWith(std::string const& s, std::string const& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

inline std::vector<std::string> splitLines(std::vector<uint8_t> const& d) {
    std::vector<std::string> lines;
    std::string cur;
    for (uint8_t c : d) {
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
        else if (c != '\r') cur += char(c);
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

inline std::vector<std::string> split(std::string const& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

inline double toNum(std::string const& s) {
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) throw Fail("angka tidak valid: " + s);
    return v;
}

// ---------- parser per format ----------

// GDR2 (.gdr2, .gdr terbaru): biner, varint LEB128, f32/f64 big-endian
inline Macro parseGdr2(std::vector<uint8_t> const& data) {
    Macro m;
    m.format = "GDR2";
    Reader r(data.data(), data.size());
    r.skip(3);                       // "GDR"
    r.varint();                      // versi
    std::string tag = r.str();       // input tag
    bool hasExt = !tag.empty();
    r.str();                         // author
    r.str();                         // description
    r.f32be();                       // duration
    r.varint();                      // game version
    m.fps = r.f64be();               // framerate
    r.varint();                      // seed
    r.varint();                      // coins
    r.u8();                          // ldm
    bool platformer = r.u8() != 0;
    r.str();                         // bot name
    r.varint();                      // bot version
    r.varint();                      // level id
    r.str();                         // level name
    r.skip(size_t(r.varint()));      // extension replay

    uint64_t deaths = r.varint();
    if (deaths > r.remaining()) throw Fail("jumlah death tidak masuk akal");
    for (uint64_t k = 0; k < deaths; k++) r.varint();

    uint64_t total = r.varint();
    uint64_t p1 = r.varint();
    uint64_t prev = 0, p1Left = p1;
    while (r.remaining() > 0 && m.inputs.size() < total) {
        uint64_t packed = r.varint();
        uint64_t delta = platformer ? (packed >> 3) : (packed >> 1);
        Input in;
        in.frame = prev + delta;
        in.button = platformer ? uint8_t((packed >> 1) & 3) : 1;
        in.down = (packed & 1) != 0;
        in.p2 = (p1Left == 0);
        if (hasExt && r.remaining() > 0) {
            uint64_t es = r.varint();
            if (es > r.remaining()) { m.inputs.push_back(in); break; }
            r.skip(size_t(es));
        }
        prev = in.frame;
        m.inputs.push_back(in);
        if (p1Left > 0 && --p1Left == 0) prev = 0;
    }
    return m;
}

// GDR1 (.gdr lama): JSON atau MessagePack
inline Macro parseGdr1(Value const& v, const char* label) {
    Macro m;
    m.format = label;
    auto const* fr = v.get("framerate");
    m.fps = fr ? fr->num(240.0) : 240.0;
    auto const* inputs = v.get("inputs");
    if (!inputs || inputs->t != Value::Arr) throw Fail("field 'inputs' tidak ditemukan");
    for (auto const& e : inputs->a) {
        Input in;
        auto const* f = e.get("frame");
        if (!f) continue;
        in.frame = uint64_t(std::max(0.0, f->num()));
        auto const* b = e.get("btn");
        in.button = b ? uint8_t(b->num(1)) : 1;
        auto const* p = e.get("2p");
        in.p2 = p ? p->boolean() : false;
        auto const* dn = e.get("down");
        in.down = dn ? dn->boolean() : false;
        m.inputs.push_back(in);
    }
    return m;
}

// Silicate 2 (.slc, header "SILL")
inline Macro parseSlc2(std::vector<uint8_t> const& data) {
    Macro m;
    m.format = "Silicate 2 (SLC)";
    Reader r(data.data(), data.size());
    r.skip(4);
    m.fps = r.f64le();
    uint64_t metaSize = r.u64le();
    if (metaSize > r.remaining()) throw Fail("ukuran meta tidak valid");
    r.skip(size_t(metaSize));
    uint64_t length = r.u64le();
    uint64_t blobCount = r.u64le();
    if (blobCount > r.remaining() / 24) throw Fail("jumlah blob tidak valid");
    struct Blob { uint64_t byteSize, start, length; };
    std::vector<Blob> blobs;
    for (uint64_t k = 0; k < blobCount; k++) {
        Blob b;
        b.byteSize = r.u64le(); b.start = r.u64le(); b.length = r.u64le();
        if (b.byteSize < 1 || b.byteSize > 8) throw Fail("ukuran input slc tidak valid");
        blobs.push_back(b);
    }
    uint64_t frame = 0;
    (void)length;
    for (auto const& b : blobs) {
        if (b.length > r.remaining() / b.byteSize + 1) throw Fail("panjang blob tidak valid");
        for (uint64_t k = 0; k < b.length; k++) {
            uint64_t state = 0;
            r.need(size_t(b.byteSize));
            for (uint64_t j = 0; j < b.byteSize; j++) state |= uint64_t(r.u8()) << (8 * j);
            frame += state >> 5;
            uint64_t button = (state & 0x1C) >> 2;
            if (button >= 1 && button <= 3) {
                Input in;
                in.frame = frame;
                in.button = uint8_t(button);
                in.down = (state & 1) != 0;
                in.p2 = (state & 2) != 0;
                m.inputs.push_back(in);
            } else if (button == 7) {
                double tps = r.f64le();
                if (tps > 0 && std::fabs(tps - m.fps) > 1e-6) m.fpsChanged = true;
            } else if (button > 7) {
                throw Fail("tipe input slc tidak dikenal");
            }
        }
    }
    return m;
}

// Silicate 3 (.slc, header "SLC3RPLY"): atom -> action atom -> section (input / repeat / special)
struct Slc3Actions {
    std::vector<Input>* out;
    Macro* macro;
    uint64_t last = 0;      // frame aksi terakhir (semua jenis aksi)
    uint64_t total = 0;     // jumlah aksi yang sudah dibaca
    uint64_t count = 0;     // jumlah aksi yang dideklarasikan atom
};

inline uint64_t slcReadN(Reader& r, size_t n) {
    uint64_t v = 0;
    r.need(n);
    for (size_t j = 0; j < n; j++) v |= uint64_t(r.u8()) << (8 * j);
    return v;
}

inline void slc3ReadSection(Reader& r, Slc3Actions& st) {
    uint16_t h = uint16_t(r.u8());
    h |= uint16_t(r.u8()) << 8;
    unsigned id = h >> 14;

    auto ensure = [&](uint64_t add) {
        if (add > st.count - st.total) throw Fail("section melebihi jumlah aksi yang dideklarasikan");
    };
    auto push = [&](uint64_t frame, uint8_t button, bool p2, bool down) {
        Input in; in.frame = frame; in.button = button; in.p2 = p2; in.down = down;
        st.out->push_back(in);
    };

    if (id == 0) {                                  // Input
        size_t byteSize = size_t(1) << ((h >> 12) & 3);
        uint64_t length = uint64_t(1) << ((h >> 8) & 15);
        for (uint64_t k = 0; k < length; k++) {
            uint64_t state = slcReadN(r, byteSize);
            uint64_t frame = st.last + (state >> 4);
            unsigned button = unsigned((state >> 2) & 3);
            bool hold = (state & 1) != 0, p2 = (state & 2) != 0;
            ensure(button == 0 ? 2 : 1);
            if (button == 0) {                      // swift: tekan + lepas di frame yang sama
                push(frame, 1, p2, true);
                push(frame, 1, p2, false);
                st.total += 2;
            } else {
                push(frame, uint8_t(button), p2, hold);
                st.total += 1;
            }
            st.last = frame;
        }
    } else if (id == 1) {                           // Repeat
        size_t byteSize = size_t(1) << ((h >> 12) & 3);
        uint64_t length = uint64_t(1) << ((h >> 8) & 15);
        unsigned repeatsExp = (h >> 3) & 31;
        uint64_t repeats = uint64_t(1) << repeatsExp;
        struct P { uint64_t delta; unsigned button; bool hold, p2; };
        std::vector<P> ps;
        uint64_t prev = 0, perRepeat = 0;
        for (uint64_t k = 0; k < length; k++) {
            uint64_t state = slcReadN(r, byteSize);
            P p;
            p.delta = state >> 4;
            p.button = unsigned((state >> 2) & 3);
            p.hold = (state & 1) != 0;
            p.p2 = (state & 2) != 0;
            prev += p.delta;
            perRepeat += p.button == 0 ? 2 : 1;
            ps.push_back(p);
        }
        if (perRepeat != 0 && repeats > (st.count - st.total) / perRepeat)
            throw Fail("section repeat melebihi jumlah aksi yang dideklarasikan");
        for (uint64_t rep = 0; rep < repeats; rep++) {
            uint64_t f = st.last;
            for (auto const& p : ps) {
                f += p.delta;
                if (p.button == 0) {
                    push(f, 1, p.p2, true);
                    push(f, 1, p.p2, false);
                    st.total += 2;
                } else {
                    push(f, uint8_t(p.button), p.p2, p.hold);
                    st.total += 1;
                }
            }
            st.last = f;
        }
    } else if (id == 2) {                           // Special
        size_t byteSize = size_t(1) << ((h >> 8) & 3);
        unsigned type = (h >> 10) & 15;
        uint64_t delta = slcReadN(r, byteSize);
        ensure(1);
        if (type == 3) {
            double tps = r.f64le();
            if (!(tps > 0) || !std::isfinite(tps)) throw Fail("TPS slc3 tidak valid");
            if (std::fabs(tps - st.macro->fps) > 1e-6) st.macro->fpsChanged = true;
        } else if (type <= 2) {
            r.skip(8);                              // seed
        } else if (type != 4) {
            throw Fail("tipe special slc3 tidak dikenal");
        }
        st.last += delta;
        st.total += 1;
    } else {
        throw Fail("section slc3 tidak valid");
    }
}

inline Macro parseSlc3(std::vector<uint8_t> const& data) {
    Macro m;
    m.format = "Silicate 3 (SLC)";
    if (data.size() < 8 + 2 + 64 + 1) throw Fail("file slc3 terlalu kecil");
    Reader r(data.data(), data.size());
    r.skip(8);
    uint16_t metaSize = uint16_t(r.u8());
    metaSize |= uint16_t(r.u8()) << 8;
    if (metaSize != 64) throw Fail("ukuran metadata slc3 tidak valid");
    m.fps = r.f64le();
    r.skip(56);                                     // seed, versi, build, dll
    size_t endPos = data.size() - 1;
    if (data.back() != 0xCC) throw Fail("footer slc3 tidak valid");

    while (r.p < endPos) {
        if (endPos - r.p < 12) throw Fail("header atom terpotong");
        uint32_t id = r.u32le();
        uint64_t sf = r.u64le();
        uint64_t size = sf & 0x00ffffffffffffffULL;
        if (size > endPos - r.p) throw Fail("body atom melewati batas file");
        size_t bodyStart = r.p;
        if (id == 1) {                              // action atom
            if (size < 8) throw Fail("action atom terlalu kecil");
            Reader sub(data.data() + bodyStart, size_t(size));
            Slc3Actions st;
            st.out = &m.inputs;
            st.macro = &m;
            st.count = sub.u64le();
            if (st.count > (uint64_t(1) << 23)) throw Fail("jumlah aksi tidak masuk akal");
            while (st.total < st.count) slc3ReadSection(sub, st);
        }
        r.p = bodyStart + size_t(size);
    }
    return m;
}

// Mega Hack Replay JSON (.mhr.json)
inline Macro parseMhrJson(Value const& v) {
    Macro m;
    m.format = "Mega Hack Replay (JSON)";
    auto const* meta = v.get("meta");
    auto const* fps = meta ? meta->get("fps") : nullptr;
    if (!fps) throw Fail("meta.fps tidak ditemukan");
    m.fps = fps->num();
    auto const* ev = v.get("events");
    if (!ev || ev->t != Value::Arr) throw Fail("field 'events' tidak ditemukan");
    for (auto const& e : ev->a) {
        auto const* f = e.get("frame");
        auto const* d = e.get("down");
        if (!f || !d) continue;
        Input in;
        in.frame = uint64_t(std::max(0.0, f->num()));
        in.down = d->boolean();
        auto const* p2 = e.get("p2");
        in.p2 = p2 ? p2->boolean() : false;
        m.inputs.push_back(in);
    }
    return m;
}

// Mega Hack Replay biner (.mhr, magic "HACK"), tiap aksi 32 byte
inline Macro parseMhrBin(std::vector<uint8_t> const& data) {
    Macro m;
    m.format = "Mega Hack Replay (biner)";
    Reader r(data.data(), data.size());
    r.skip(12);
    m.fps = r.u32le();
    r.skip(12);                       // sampai offset 28
    uint32_t count = r.u32le();
    if (count > r.remaining() / 32 + 1) throw Fail("jumlah aksi tidak valid");
    for (uint32_t k = 0; k < count; k++) {
        r.skip(2);
        Input in;
        in.down = r.u8() == 1;
        in.p2 = r.u8() != 0;
        in.frame = r.u32le();
        r.skip(24);
        m.inputs.push_back(in);
    }
    return m;
}

// zBot Frame (.zbf)
inline Macro parseZbf(std::vector<uint8_t> const& data) {
    Macro m;
    m.format = "zBot Frame (ZBF)";
    Reader r(data.data(), data.size());
    float delta = r.f32le();
    float speed = r.f32le();
    if (speed == 0.f) speed = 1.f;
    if (delta <= 0.f) throw Fail("delta zbf tidak valid");
    m.fps = 1.0 / double(delta) / double(speed);
    while (r.remaining() >= 6) {
        Input in;
        int32_t frame = int32_t(r.u32le());
        in.frame = frame < 0 ? 0 : uint64_t(frame);
        in.down = r.u8() == 0x31;
        in.p2 = r.u8() != 0x31;
        m.inputs.push_back(in);
    }
    return m;
}

// Plaintext (.txt): baris pertama FPS, lalu "frame down button p1(1)/p2(0)"
inline Macro parsePlain(std::vector<uint8_t> const& data) {
    Macro m;
    m.format = "Plaintext (TXT)";
    auto lines = splitLines(data);
    if (lines.empty()) throw Fail("file kosong");
    m.fps = toNum(lines[0]);
    for (size_t k = 1; k < lines.size(); k++) {
        auto f = split(lines[k], ' ');
        if (f.size() < 4) continue;
        Input in;
        double fr = toNum(f[0]);
        in.frame = fr < 0 ? 0 : uint64_t(fr);
        in.down = toNum(f[1]) == 1;
        in.button = uint8_t(toNum(f[2]));
        in.p2 = toNum(f[3]) == 0;
        m.inputs.push_back(in);
    }
    return m;
}

// xdBot teks (.xd): baris FPS lalu "frame|holding|button|player1|..."
inline Macro parseXd(std::vector<uint8_t> const& data) {
    Macro m;
    m.format = "xdBot (XD, teks)";
    m.fps = 240.0;
    for (auto const& line : splitLines(data)) {
        if (line.empty()) continue;
        auto f = split(line, '|');
        if (f.size() == 1) { m.fps = toNum(f[0]); continue; }
        if (f.size() < 4) continue;
        Input in;
        double fr = toNum(f[0]);
        in.frame = fr < 0 ? 0 : uint64_t(fr);
        in.down = toNum(f[1]) == 1;
        in.button = uint8_t(toNum(f[2]));
        in.p2 = toNum(f[3]) != 1;
        m.inputs.push_back(in);
    }
    return m;
}

} // namespace detail

// Deteksi format dari isi file (magic) lalu ekstensi, kemudian parse.
inline Result parse(std::vector<uint8_t> const& data, std::string const& filename) {
    using namespace detail;
    Result res;
    try {
        if (data.empty()) throw Fail("file kosong");
        std::string name = lower(filename);
        Macro m;

        size_t firstNonWs = 0;
        while (firstNonWs < data.size() && std::isspace(data[firstNonWs])) firstNonWs++;
        bool looksJson = firstNonWs < data.size() && data[firstNonWs] == '{';

        if (startsWith(data, "GDR")) {
            m = parseGdr2(data);
        } else if (startsWith(data, "SLC3RPLY")) {
            m = parseSlc3(data);
        } else if (startsWith(data, "SILL")) {
            m = parseSlc2(data);
        } else if (startsWith(data, "HACK")) {
            m = parseMhrBin(data);
        } else if (looksJson) {
            JsonParser jp(data.data(), data.size());
            Value v = jp.value();
            if (v.get("events")) m = parseMhrJson(v);
            else if (v.get("inputs")) m = parseGdr1(v, "GDR1 (JSON)");
            else throw Fail("JSON dikenali tetapi bukan format macro yang didukung");
        } else if (endsWith(name, ".zbf")) {
            m = parseZbf(data);
        } else if (endsWith(name, ".xd")) {
            m = parseXd(data);
        } else if (endsWith(name, ".txt")) {
            bool pipes = std::find(data.begin(), data.begin() + std::min<size_t>(data.size(), 400), uint8_t('|'))
                         != data.begin() + std::min<size_t>(data.size(), 400);
            m = pipes ? parseXd(data) : parsePlain(data);
        } else {
            // GDR1 MessagePack
            MsgPack mp(data.data(), data.size());
            Value v = mp.value();
            if (v.get("inputs")) m = parseGdr1(v, "GDR1 (MessagePack)");
            else throw Fail("format tidak dikenali");
        }

        if (m.fps <= 0.0 || !std::isfinite(m.fps)) throw Fail("FPS/TPS macro tidak valid");
        std::stable_sort(m.inputs.begin(), m.inputs.end(),
                         [](Input const& a, Input const& b) { return a.frame < b.frame; });
        res.macro = std::move(m);
        res.ok = true;
    } catch (std::exception const& e) {
        res.ok = false;
        res.error = e.what();
    }
    return res;
}

// Hitung panjang tiap hold (tekan -> lepas) menjadi histogram.
// refFps > 0: konversi ke frame pada FPS acuan; 0: pakai frame asli macro.
inline Stats analyze(Macro const& m, double refFps) {
    Stats st;
    st.fps = m.fps;
    std::map<std::pair<bool, int>, uint64_t> held;
    for (auto const& in : m.inputs) {
        auto key = std::make_pair(in.p2, int(in.button));
        if (in.down) {
            st.presses++;
            if (!held.count(key)) held[key] = in.frame;
        } else {
            auto it = held.find(key);
            if (it == held.end()) continue;
            double dur = double(in.frame - it->second);
            if (refFps > 0 && m.fps > 0) dur = dur * refFps / m.fps;
            long f = std::lround(dur);
            int frames = f < 1 ? 1 : int(f);
            st.hist[frames]++;
            st.holds++;
            st.longest = std::max(st.longest, frames);
            held.erase(it);
        }
    }
    return st;
}

} // namespace pmacro
