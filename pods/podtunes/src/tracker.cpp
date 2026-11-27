// Pod Tunes - model, synth, sequencer, JSON and WAV export. See tracker.h.
#include "tracker.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "pd/fs.h"

namespace podtunes {

const char* const kChannelNames[kChannels] = {"LEAD", "ARP", "BASS", "DRM"};
const char* const kWaveNames[4] = {"SQ25", "SQ50", "SQ12", "TRI"};
const char* const kDrumNames[3] = {"KICK", "SNAR", "HAT"};
const char* const kDrumJson[3] = {"kick", "snare", "hat"};
const int kChannelBase[3] = {60, 60, 36};            // C4, C4, C2
const float kChannelVol[kChannels] = {0.16f, 0.055f, 0.24f, 1.0f};

// ---------------------------------------------------------------- music theory
static const char* const kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

static int semi_of(char letter) {
    switch (letter) {
        case 'C': return 0; case 'D': return 2; case 'E': return 4; case 'F': return 5;
        case 'G': return 7; case 'A': return 9; case 'B': return 11;
    }
    return 0;
}

int note_to_midi(const std::string& n) {
    if (n.size() >= 2 && n[0] >= 'A' && n[0] <= 'G') {
        int s = semi_of(n[0]);
        size_t i = 1;
        if (i < n.size() && (n[i] == '#' || n[i] == 'b')) {
            s += n[i] == '#' ? 1 : -1;
            ++i;
        }
        if (i < n.size() && n[i] >= '0' && n[i] <= '9')
            return (n[i] - '0' + 1) * 12 + s;
    }
    return 60;
}

std::string midi_to_note(int m) {
    m = m < 0 ? 0 : m;
    return std::string(kNames[m % 12]) + std::to_string(m / 12 - 1);
}

double note_freq(int m) { return 440.0 * std::pow(2.0, (m - 69) / 12.0); }

// ---------------------------------------------------------------- model
SongPos song_pos(const Song& s, bool pattern_mode, int cur_pat, long abs_step) {
    if (pattern_mode || s.order.empty())
        return {cur_pat, int(((abs_step % kSteps) + kSteps) % kSteps)};
    long total = (long)s.order.size() * kSteps;
    long st = ((abs_step % total) + total) % total;
    int p = s.order[st / kSteps];
    if (p < 0 || p >= kPatterns) p = cur_pat;
    return {p, int(st % kSteps)};
}

long step_samples(const Song& s, int sample_rate) {
    double d = 60.0 / (s.tempo > 0 ? s.tempo : 152) / 4.0 * sample_rate;
    return d > 1.0 ? long(d + 0.5) : 1;
}

bool ch_audible(const EditState& e, int ch) {
    bool any_solo = false;
    for (int c = 0; c < kChannels; ++c)
        if (e.ch[c].solo) any_solo = true;
    return any_solo ? e.ch[ch].solo : e.ch[ch].on;
}

float eff_vol(const EditState& e, int ch) {
    return kChannelVol[ch] * float(e.ch[ch].vol) / 100.0f;
}

void Pattern::clear() {
    for (int c = 0; c < 3; ++c)
        for (int st = 0; st < kSteps; ++st) mel[c][st] = Note{};
    for (int st = 0; st < kSteps; ++st) drums[st] = DRUM_NONE;
}

void Song::reset() {
    name = "UNTITLED";
    tempo = 152;
    order.clear();
    for (int p = 0; p < kPatterns; ++p) patterns[p].clear();
}

void build_demo(Song& s) {
    struct Lead { const char* n; int len; };
    static const Lead LEAD[] = {
        {"E5", 2}, {"G5", 2}, {"C6", 2}, {"E6", 2}, {"D6", 2}, {"C6", 2}, {"G5", 2}, {"E5", 2},
        {"G5", 1}, {"A5", 1}, {"G5", 1}, {"E5", 1}, {"C5", 2}, {"E5", 2}, {"G5", 2}, {"B5", 2}, {"C6", 4},
        {"F5", 2}, {"A5", 2}, {"C6", 4}, {"D6", 2}, {"C6", 2}, {"A5", 2}, {"F5", 2},
        {"B5", 2}, {"D6", 2}, {"B5", 2}, {"G5", 2}, {"A5", 2}, {"B5", 2}, {"D6", 4},
        {"A5", 2}, {"C6", 2}, {"E6", 2}, {"C6", 2}, {"B5", 2}, {"A5", 2}, {"B5", 2}, {"E5", 2},
        {"C6", 1}, {"B5", 1}, {"A5", 1}, {"B5", 1}, {"C6", 2}, {"E6", 2}, {"D6", 2}, {"C6", 2}, {"B5", 2}, {"A5", 2},
        {"A5", 2}, {"F5", 2}, {"C6", 2}, {"F6", 2}, {"E6", 2}, {"F6", 2}, {"C6", 2}, {"A5", 2},
        {"D6", 2}, {"C6", 2}, {"B5", 2}, {"A5", 2}, {"G5", 2}, {"A5", 2}, {"B5", 2}, {"D6", 2},
    };
    struct Chord {
        int bass;
        int arp[3];
    };
    static const Chord CHORDS[4] = {   // C, F, G, Am
        {note_to_midi("C2"), {note_to_midi("C4"), note_to_midi("E4"), note_to_midi("G4")}},
        {note_to_midi("F2"), {note_to_midi("F3"), note_to_midi("A3"), note_to_midi("C4")}},
        {note_to_midi("G2"), {note_to_midi("G3"), note_to_midi("B3"), note_to_midi("D4")}},
        {note_to_midi("A2"), {note_to_midi("A3"), note_to_midi("C4"), note_to_midi("E4")}},
    };
    static const int PROG[8] = {0, 0, 1, 2, 3, 3, 1, 2};   // C C F G Am Am F G
    static const int ARP_PAT[16] = {0, 1, 2, 1, 0, 1, 2, 1, 0, 1, 2, 1, 0, 1, 2, 1};

    s.reset();
    s.name = "PIXEL PLAINS ZONE";
    s.tempo = 152;
    s.order = {0, 1, 2, 3};
    for (int p = 0; p < kPatterns; ++p) s.patterns[p].clear();

    long step = 0;
    for (const Lead& l : LEAD) {
        long abs = step;
        if (abs < 4 * kSteps) {
            int pi = int(abs / kSteps), pos = int(abs % kSteps);
            s.patterns[pi].mel[CH_LEAD][pos] = {uint8_t(note_to_midi(l.n)), uint8_t(l.len)};
        }
        step += l.len;
    }
    for (long a = 0; a < 4 * kSteps; ++a) {
        int pi = int(a / kSteps), pos = int(a % kSteps), bpos = int(a % 16), bar = int(a / 16);
        const Chord& ch = CHORDS[PROG[bar]];
        s.patterns[pi].mel[CH_ARP][pos] = {uint8_t(ch.arp[ARP_PAT[bpos]]), 1};
        if (bpos % 2 == 0) {
            int m = ch.bass + (bpos % 4 == 2 ? 12 : 0);
            s.patterns[pi].mel[CH_BASS][pos] = {uint8_t(m), 2};
        }
        if (bpos == 0 || bpos == 8)
            s.patterns[pi].drums[pos] = DRUM_KICK;
        else if (bpos == 4 || bpos == 12 || (bar == 7 && bpos == 15))
            s.patterns[pi].drums[pos] = DRUM_SNARE;
        else if (bpos % 2 == 0)
            s.patterns[pi].drums[pos] = DRUM_HAT;
    }
}

std::string slug(const std::string& s) {
    std::string out;
    for (char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            out += c;
        else if (c >= 'A' && c <= 'Z')
            out += char(c - 'A' + 'a');
        else if (!out.empty() && out.back() != '-')
            out += '-';
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "song" : out;
}

// ---------------------------------------------------------------- JSON out
static void json_escape(std::string& out, const std::string& s) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (uint8_t(c) < 0x20) {
                    char b[8];
                    snprintf(b, sizeof b, "\\u%04x", c);
                    out += b;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

std::string song_to_json(const Song& s, bool pretty) {
    std::string o;
    auto nl = [&](int ind) {
        if (pretty) {
            o += '\n';
            o.append(size_t(ind) * 2, ' ');
        }
    };
    auto colon = [&]() { o += pretty ? ": " : ":"; };
    o += '{';
    o += "\"app\":\"pod-tunes\",\"version\":1,\"song\":{";
    nl(1); o += "\"name\""; colon(); json_escape(o, s.name); o += ',';
    nl(1); o += "\"tempo\""; colon(); o += std::to_string(s.tempo); o += ',';
    nl(1); o += "\"order\""; colon(); o += '[';
    for (size_t i = 0; i < s.order.size(); ++i) {
        if (i) o += ',';
        o += std::to_string(s.order[i]);
    }
    o += "],";
    nl(1); o += "\"patterns\""; colon(); o += '[';
    for (int p = 0; p < kPatterns; ++p) {
        if (p) o += ',';
        nl(2); o += '{';
        for (int c = 0; c < 4; ++c) {
            if (c) o += ',';
            nl(3); o += '"';
            o += c == 0 ? "lead" : c == 1 ? "arp" : c == 2 ? "bass" : "drums";
            o += '"'; colon(); o += '{';
            bool first = true;
            for (int st = 0; st < kSteps; ++st) {
                if (c < 3) {
                    const Note& n = s.patterns[p].mel[c][st];
                    if (!n.midi) continue;
                    if (!first) o += ',';
                    first = false;
                    nl(4); o += '"' + std::to_string(st) + "\""; colon();
                    o += "{\"n\""; colon(); json_escape(o, midi_to_note(n.midi));
                    o += ",\"l\""; colon(); o += std::to_string(n.len) + '}';
                } else {
                    uint8_t d = s.patterns[p].drums[st];
                    if (!d) continue;
                    if (!first) o += ',';
                    first = false;
                    nl(4); o += '"' + std::to_string(st) + "\""; colon();
                    o += '"'; o += kDrumJson[d - 1]; o += '"';
                }
            }
            nl(3); o += '}';
        }
        nl(2); o += '}';
    }
    nl(1); o += ']';
    nl(0); o += '}';
    nl(0); o += '}';
    if (pretty) o += '\n';
    return o;
}

// ---------------------------------------------------------------- JSON in
namespace {

struct JV {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<JV> arr;
    std::vector<std::pair<std::string, JV>> obj;
    const JV* find(const char* key) const {
        if (type != Obj) return nullptr;
        for (const auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
};

struct JParser {
    const char* p;
    const char* end;
    void skip() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }
    bool parse_string(std::string& out) {
        if (p >= end || *p != '"') return false;
        ++p;
        out.clear();
        while (p < end && *p != '"') {
            char c = *p++;
            if (c == '\\' && p < end) {
                char e = *p++;
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case '/': out += '/'; break;
                    case '\\': out += '\\'; break;
                    case '"': out += '"'; break;
                    case 'u': {
                        if (end - p < 4) return false;
                        unsigned cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = *p++;
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= unsigned(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= unsigned(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= unsigned(h - 'A' + 10);
                            else return false;
                        }
                        if (cp < 0x80) out += char(cp);
                        else if (cp < 0x800) {
                            out += char(0xC0 | (cp >> 6));
                            out += char(0x80 | (cp & 63));
                        } else {
                            out += char(0xE0 | (cp >> 12));
                            out += char(0x80 | ((cp >> 6) & 63));
                            out += char(0x80 | (cp & 63));
                        }
                        break;
                    }
                    default: return false;
                }
            } else {
                out += c;
            }
        }
        if (p >= end) return false;
        ++p;
        return true;
    }
    bool parse(JV& v) {
        skip();
        if (p >= end) return false;
        char c = *p;
        if (c == '{') {
            ++p;
            v.type = JV::Obj;
            skip();
            if (p < end && *p == '}') { ++p; return true; }
            for (;;) {
                skip();
                std::string key;
                if (!parse_string(key)) return false;
                skip();
                if (p >= end || *p != ':') return false;
                ++p;
                JV val;
                if (!parse(val)) return false;
                v.obj.emplace_back(std::move(key), std::move(val));
                skip();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++p;
            v.type = JV::Arr;
            skip();
            if (p < end && *p == ']') { ++p; return true; }
            for (;;) {
                JV val;
                if (!parse(val)) return false;
                v.arr.push_back(std::move(val));
                skip();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; return true; }
                return false;
            }
        }
        if (c == '"') {
            v.type = JV::Str;
            return parse_string(v.str);
        }
        if (c == 't' && end - p >= 4 && !strncmp(p, "true", 4)) {
            p += 4;
            v.type = JV::Bool;
            v.b = true;
            return true;
        }
        if (c == 'f' && end - p >= 5 && !strncmp(p, "false", 5)) {
            p += 5;
            v.type = JV::Bool;
            v.b = false;
            return true;
        }
        if (c == 'n' && end - p >= 4 && !strncmp(p, "null", 4)) {
            p += 4;
            v.type = JV::Null;
            return true;
        }
        char* e = nullptr;
        double d = strtod(p, &e);
        if (e == p) return false;
        p = e;
        v.type = JV::Num;
        v.num = d;
        return true;
    }
};

}  // namespace

bool song_from_json(const std::string& text, Song& out) {
    JParser jp{text.c_str(), text.c_str() + text.size()};
    JV root;
    if (!jp.parse(root)) return false;
    jp.skip();
    if (jp.p != jp.end) return false;
    const JV* s = root.find("song");
    if (!s) s = &root;
    if (s->type != JV::Obj) return false;

    static Song ns;   // static: a Song is ~2 KB, too big for the device stack
    ns.reset();
    if (const JV* v = s->find("name"); v && v->type == JV::Str) ns.name = v->str.substr(0, 24);
    if (ns.name.empty()) ns.name = "UNTITLED";
    if (const JV* v = s->find("tempo"); v && v->type == JV::Num) {
        int t = int(v->num);
        ns.tempo = t < 40 ? 40 : t > 400 ? 400 : t;
    }
    if (const JV* o = s->find("order"); o && o->type == JV::Arr) {
        for (const JV& x : o->arr) {
            if (x.type != JV::Num) continue;
            int p = int(x.num);
            if (p >= 0 && p < kPatterns && (int)ns.order.size() < kMaxOrder) ns.order.push_back(p);
        }
    }
    if (ns.order.empty()) ns.order.push_back(0);
    if (const JV* pats = s->find("patterns"); pats && pats->type == JV::Arr) {
        for (int i = 0; i < kPatterns && i < (int)pats->arr.size(); ++i) {
            const JV& src = pats->arr[i];
            if (src.type != JV::Obj) continue;
            Pattern& pat = ns.patterns[i];
            for (int c = 0; c < 3; ++c) {
                const char* keyname = c == 0 ? "lead" : c == 1 ? "arp" : "bass";
                const JV* o = src.find(keyname);
                if (!o || o->type != JV::Obj) continue;
                for (const auto& kv : o->obj) {
                    int st = atoi(kv.first.c_str());
                    if (st < 0 || st >= kSteps || kv.second.type != JV::Obj) continue;
                    const JV* n = kv.second.find("n");
                    if (!n || n->type != JV::Str) continue;
                    int midi = note_to_midi(n->str);
                    if (midi < 1 || midi > 127) continue;
                    int len = 1;
                    if (const JV* l = kv.second.find("l"); l && l->type == JV::Num) len = int(l->num);
                    if (len < 1) len = 1;
                    if (len > kSteps) len = kSteps;
                    pat.mel[c][st] = {uint8_t(midi), uint8_t(len)};
                }
            }
            const JV* o = src.find("drums");
            if (o && o->type == JV::Obj) {
                for (const auto& kv : o->obj) {
                    int st = atoi(kv.first.c_str());
                    if (st < 0 || st >= kSteps || kv.second.type != JV::Str) continue;
                    const std::string& d = kv.second.str;
                    for (int k = 0; k < 3; ++k)
                        if (d == kDrumJson[k]) pat.drums[st] = uint8_t(k + 1);
                }
            }
        }
    }
    out = ns;
    return true;
}

// ---------------------------------------------------------------- synth
void Synth::start(int sample_rate) { sr_ = sample_rate > 0 ? sample_rate : 22050; }

void Synth::note_on(int ch, uint8_t midi, int wave, uint64_t at_smp, uint64_t dur_smp, float vol) {
    Voice& v = voices_[ch & 3];
    v = Voice{};
    v.on = true;
    v.ch = ch & 3;
    v.wave = wave;
    v.freq = note_freq(midi);
    v.start = at_smp;
    v.dur = dur_smp ? dur_smp : 1;
    v.vol = vol;
}

void Synth::drum_on(int drum, uint64_t at_smp, float vol) {
    static const double kDur[4] = {0, 0.13, 0.09, 0.025};
    Voice& v = voices_[CH_DRUMS];
    v = Voice{};
    v.on = true;
    v.ch = CH_DRUMS;
    v.drum = true;
    v.type = drum;
    v.start = at_smp;
    v.dur = uint64_t(kDur[drum & 3] * sr_);
    v.vol = vol;
}

void Synth::audition(int ch, uint8_t midi, int wave) {
    Voice* slot = &voices_[4];
    for (int i = 4; i < 8; ++i)
        if (!voices_[i].on) { slot = &voices_[i]; break; }
    *slot = Voice{};
    slot->on = true;
    slot->ch = ch & 3;
    slot->wave = wave;
    slot->freq = note_freq(midi);
    slot->start = now_smp_;
    slot->dur = uint64_t(0.18 * sr_);
    slot->vol = kChannelVol[ch & 3];
}

void Synth::audition_drum(int drum) {
    Voice* slot = &voices_[4];
    for (int i = 4; i < 8; ++i)
        if (!voices_[i].on) { slot = &voices_[i]; break; }
    static const float kMul[4] = {0, 0.5f, 0.2f, 0.07f};
    *slot = Voice{};
    slot->on = true;
    slot->ch = CH_DRUMS;
    slot->drum = true;
    slot->type = drum;
    slot->start = now_smp_;
    slot->dur = uint64_t((drum == DRUM_KICK ? 0.13 : drum == DRUM_SNARE ? 0.09 : 0.025) * sr_);
    slot->vol = kMul[drum & 3];
}

void Synth::clear() {
    for (int i = 0; i < 8; ++i) voices_[i].on = false;
}

float Synth::voice_sample(Voice& v, uint64_t age_smp) {
    const double inv = 1.0 / sr_;
    if (v.drum) {
        rng_ = rng_ * 1664525u + 1013904223u;
        double n = double(int32_t(rng_)) / 4294967296.0 * 2.0;   // -1..1
        double hp = n - v.prev_n;
        v.prev_n = n;
        double dur_s = double(v.dur) * inv;
        double env = std::pow(0.0001, double(age_smp) * inv / (v.type == DRUM_KICK ? 0.13 : v.type == DRUM_SNARE ? 0.09 : 0.025));
        if (v.type == DRUM_KICK) {
            double f = 45.0 + 115.0 * std::pow(45.0 / 160.0, double(age_smp) * inv / 0.10);
            v.phase += f * inv;
            if (v.phase >= 1.0) v.phase -= 1.0;
            return float(std::sin(6.283185307179586 * v.phase) * env);
        }
        (void)dur_s;
        return float(hp * env);
    }
    v.phase += v.freq * inv;
    if (v.phase >= 1.0) v.phase -= 1.0;
    float out;
    if (v.wave == WAVE_TRI) {
        out = float(4.0 * std::fabs(v.phase - 0.5) - 1.0);
    } else {
        double duty = v.wave == WAVE_SQ50 ? 0.5 : v.wave == WAVE_SQ12 ? 0.125 : 0.25;
        out = v.phase < duty ? 1.0f : -1.0f;
    }
    double a = double(age_smp) / (0.004 * sr_);
    if (a > 1.0) a = 1.0;
    double r = (double(v.dur) - double(age_smp)) / (0.015 * sr_);
    if (r > 1.0) r = 1.0;
    if (r < 0.0) r = 0.0;
    return float(out * a * r);
}

void Synth::render(int frames, int16_t* out) {
    float peak = 0;
    float lv[4] = {0, 0, 0, 0};
    for (int i = 0; i < frames; ++i) {
        uint64_t t = now_smp_ + uint64_t(i);
        float mix = 0;
        for (int k = 0; k < 8; ++k) {
            Voice& v = voices_[k];
            if (!v.on || t < v.start || t >= v.start + v.dur) continue;
            float s = voice_sample(v, t - v.start);
            mix += s * v.vol;
            float a = s < 0 ? -s : s;
            if (a > lv[v.ch]) lv[v.ch] = a;
        }
        if (mix > 1.0f) mix = 1.0f;
        else if (mix < -1.0f) mix = -1.0f;
        int16_t s16 = int16_t(mix * 32767.0f * 0.9f);
        out[i * 2] = s16;
        out[i * 2 + 1] = s16;
        float a = mix < 0 ? -mix : mix;
        if (a > peak) peak = a;
    }
    for (int c = 0; c < 4; ++c)
        ch_level_[c] = lv[c] > ch_level_[c] ? lv[c] : ch_level_[c] * 0.85f;
    peak_ = peak > peak_ ? peak : peak_ * 0.8f;
    now_smp_ += uint64_t(frames);
}

// ---------------------------------------------------------------- sequencer
void schedule_step(const Song& s, const EditState& e, Synth& synth, uint64_t at_smp, long abs_step) {
    SongPos p = song_pos(s, e.pattern_mode, e.cur_pat, abs_step);
    const Pattern& pat = s.patterns[p.pat];
    long ss = step_samples(s, synth.sample_rate());
    for (int c = 0; c < 3; ++c) {
        const Note& n = pat.mel[c][p.step];
        if (!n.midi || !ch_audible(e, c)) continue;
        long len = n.len < kSteps - p.step ? n.len : kSteps - p.step;
        synth.note_on(c, n.midi, e.ch[c].wave, at_smp, uint64_t(len) * uint64_t(ss) * 92 / 100,
                      eff_vol(e, c));
    }
    uint8_t d = pat.drums[p.step];
    if (d && ch_audible(e, CH_DRUMS)) {
        static const float kMul[4] = {0, 0.5f, 0.2f, 0.07f};
        synth.drum_on(d, at_smp, eff_vol(e, CH_DRUMS) * kMul[d]);
    }
}

void Player::attach(const Song* s, const EditState* e, Synth* synth) {
    song_ = s;
    edit_ = e;
    synth_ = synth;
}

long Player::step_smp() const { return step_samples(*song_, synth_->sample_rate()); }

void Player::play(long from_step) {
    playing_ = true;
    sched_step_ = from_step;
    anchor_ = synth_->now() + uint64_t(synth_->sample_rate()) / 16;
}

void Player::stop() {
    playing_ = false;
    if (synth_) synth_->clear();
}

void Player::schedule_ahead(uint64_t horizon_smp) {
    if (!playing_ || !song_ || !synth_) return;
    long ss = step_smp();
    while (anchor_ + uint64_t(sched_step_) * uint64_t(ss) < synth_->now() + horizon_smp) {
        schedule_step(*song_, *edit_, *synth_, anchor_ + uint64_t(sched_step_) * uint64_t(ss), sched_step_);
        ++sched_step_;
    }
}

SongPos Player::pos() const {
    if (!playing_ || !song_ || !synth_) return {edit_->cur_pat, 0};
    long ss = step_smp();
    if (ss <= 0) return {edit_->cur_pat, 0};
    long ph = long((synth_->now() - anchor_) / uint64_t(ss));
    if (ph < 0) ph = 0;
    return song_pos(*song_, edit_->pattern_mode, edit_->cur_pat, ph);
}

// ---------------------------------------------------------------- WAV export
namespace {

void put_u32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
}

void put_u16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}

void wav_header(uint8_t* h, uint32_t sr, uint16_t ch, uint32_t data_bytes) {
    memcpy(h, "RIFF", 4);
    put_u32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    put_u32(h + 16, 16);
    put_u16(h + 20, 1);
    put_u16(h + 22, ch);
    put_u32(h + 24, sr);
    put_u32(h + 28, sr * ch * 2);
    put_u16(h + 32, uint16_t(ch * 2));
    put_u16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put_u32(h + 40, data_bytes);
}

}  // namespace

bool export_wav(const Song& s, const EditState& e, const std::string& path,
                const std::function<bool(long, long)>& progress) {
    const int sr = 22050;
    Synth synth;
    synth.start(sr);
    long total = (e.pattern_mode || s.order.empty()) ? long(kSteps) * 4 : long(s.order.size()) * kSteps;
    if (total < 1) total = kSteps;
    long ss = step_samples(s, sr);
    uint64_t tail = uint64_t(sr) * 3 / 5;   // 0.6 s of ring-out

    pd::fs::File f;
    if (!f.open(path, pd::fs::Mode::Write)) return false;
    uint8_t hdr[44];
    wav_header(hdr, sr, 2, 0);
    f.write(hdr, 44);

    std::vector<int16_t> buf(512 * 2);
    uint64_t written = 0;
    bool ok = true;
    for (long step = 0; step < total && ok; ++step) {
        schedule_step(s, e, synth, uint64_t(step) * uint64_t(ss), step);
        long left = ss;
        while (left > 0) {
            int n = left > 512 ? 512 : int(left);
            synth.render(n, buf.data());
            if (f.write(buf.data(), uint32_t(n) * 4) != n * 4) ok = false;
            written += uint64_t(n);
            left -= n;
        }
        if (progress && !progress(step + 1, total)) ok = false;
    }
    for (uint64_t left = tail; ok && left > 0;) {
        int n = left > 512 ? 512 : int(left);
        synth.render(n, buf.data());
        if (f.write(buf.data(), uint32_t(n) * 4) != n * 4) ok = false;
        written += uint64_t(n);
        left -= uint64_t(n);
    }
    if (ok) {
        wav_header(hdr, sr, 2, uint32_t(written) * 4);
        if (f.seek(0)) f.write(hdr, 44);
    }
    f.close();
    if (!ok) pd::fs::remove(path.c_str());
    return ok;
}

}  // namespace podtunes
