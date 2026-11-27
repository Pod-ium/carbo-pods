//
// Pod Tunes - an 8-bit music creator (chiptune tracker) for the PicoCalc,
// modelled on the "Pixel Plains" web tracker: 4 channels (lead, arp, bass,
// drums), 8 patterns of 32 steps, a song order, square/triangle waves, a
// piano-roll grid played from the keyboard, JSON song files and WAV export.
// Audio goes to the terminal's PWM output.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pd/diag.h"
#include "pd/pd.h"

#include "tracker.h"

namespace {

using namespace podtunes;
namespace key = pd::key;
namespace ui = pd::ui;
namespace gfx = pd::gfx;

const char* const kStages[] = {"start", "display", "SD card", "settings", "ready"};
enum Stage { kStart, kDisplay, kSd, kSettings, kReady };

const char* const kWorkDir = "/.podtunes";
const char* const kSettingsPath = "/.podtunes/settings.txt";
const char* const kSongPath = "/.podtunes/song.json";
constexpr int kSampleRate = 22050;
constexpr int kAudioBlock = 128;
constexpr int kVizBars = 48;

// The CRT palette of the web original.
const pd::Color kBg = 0x0b0417, kScreen = 0x05020c, kBorder = 0x6a4bd6;
const pd::Color kLine0 = 0x241243, kLine4 = 0x3d2a70, kLine16 = 0x6a4bd6;
const pd::Color kRowA = 0x130a26, kRowB = 0x0d0618;
const pd::Color kYellow = 0xffe680, kCyan = 0x7be3ff, kRed = 0xff5c5c, kGreen = 0xb8ff38;
const pd::Color kDim = 0x9f8ad6, kWhite = 0xffffff, kPlayhead = 0x2e2a44;
const pd::Color kChColor[4] = {kYellow, kCyan, kRed, kGreen};
const pd::Color kVizColors[5] = {0x38ff6e, 0xb8ff38, 0xffe680, 0xffb338, 0xff5c5c};

const char* const kThemeNames[3] = {"Night", "Paper", "Phosphor"};
const char* const kModeNames[2] = {"SONG", "PATTERN"};
const char* const kLenNames[4] = {"1", "2", "4", "8"};
const char* const kHint = "SPC play \xC2\xB7 arrows move \xC2\xB7 Z-M Q-U notes \xC2\xB7 ESC menu";

struct Settings {
    int theme = 0;
    int volume = 70;
};

struct App {
    pd::Terminal& term;
    ui::Context& ui;
    pd::app::StatusBar bar;
    Settings set;
    Song song;
    EditState ed;
    Synth synth;
    Player player;
    pd::hal::AudioOut* audio = nullptr;
    bool dirty = true;
    bool song_dirty = false;
    uint32_t last_edit_ms = 0;
    std::string toast;
    uint32_t toast_until = 0;
    float viz[kVizBars] = {};
    int viz_i = 0;
    int16_t audio_buf[kAudioBlock * 2];

    App(pd::Terminal& t, ui::Context& u) : term(t), ui(u) {}

    void load_settings() {
        pd::Settings st;
        st.load(kSettingsPath);
        set.theme = st.get_int("theme", 0, 0, ui::kThemeCount - 1);
        set.volume = st.get_int("volume", 70, 0, 100);
    }
    void save_settings() {
        if (!pd::fs::mounted()) return;
        pd::fs::mkdirs(kWorkDir);
        pd::Settings st;
        st.set_int("theme", set.theme);
        st.set_int("volume", set.volume);
        st.save(kSettingsPath);
    }
};

int rows_of(const App& a) { return a.ed.edit_ch == CH_DRUMS ? 3 : kRows; }

int keymap_offset(char c) {
    switch (c) {
        case 'z': return 0;  case 's': return 1;  case 'x': return 2;  case 'd': return 3;
        case 'c': return 4;  case 'v': return 5;  case 'g': return 6;  case 'b': return 7;
        case 'h': return 8;  case 'n': return 9;  case 'j': return 10; case 'm': return 11;
        case 'q': return 12; case '2': return 13; case 'w': return 14; case '3': return 15;
        case 'e': return 16; case 'r': return 17; case '5': return 18; case 't': return 19;
        case '6': return 20; case 'y': return 21; case '7': return 22; case 'u': return 23;
        case 'i': return 24; case '9': return 25; case 'o': return 26; case '0': return 27;
        case 'p': return 28;
    }
    return -1;
}

void toast(App& a, const std::string& m) {
    a.toast = m;
    a.toast_until = pd::sys::now_ms() + 2600;
    a.dirty = true;
}

void touch(App& a) {
    a.song_dirty = true;
    a.last_edit_ms = pd::sys::now_ms();
    a.dirty = true;
}

void save_song(App& a) {
    if (!pd::fs::mounted()) return;
    pd::fs::mkdirs(kWorkDir);
    pd::fs::write_file(kSongPath, song_to_json(a.song, false));
}

// ---------------------------------------------------------------- audio pump
void refill_block(App& a) {
    a.player.schedule_ahead(kAudioBlock + a.player.step_smp());
    a.synth.render(kAudioBlock, a.audio_buf);
    a.viz[a.viz_i % kVizBars] = a.synth.peak();
    ++a.viz_i;
    if (a.audio) a.audio->push(a.audio_buf, kAudioBlock);
}

void pump(App& a) {
    if (a.audio) {
        for (;;) {
            int space = a.audio->capacity() - a.audio->buffered();
            if (space < kAudioBlock) break;
            refill_block(a);
        }
    } else {
        // no audio output: pace the sequencer on the wall clock anyway
        uint64_t target = uint64_t(pd::sys::now_us()) * kSampleRate / 1000000;
        int guard = 64;
        while (a.synth.now() < target && guard-- > 0) refill_block(a);
    }
}

// ---------------------------------------------------------------- layout
struct Geom {
    int W, H, line_h;
    int viz_y, viz_h, lineA_y, head_y, grid_y, lineB_y, msg_y;
    int gut_x, grid_x, col_w, right_x, rows, row_h;
};

Geom geom_of(const App& a) {
    Geom g;
    g.W = gfx::width();
    g.H = gfx::height();
    const gfx::Font& f = gfx::font(gfx::FONT_6x12);
    g.line_h = f.h;
    int bar_h = a.bar.height();
    g.viz_y = bar_h;
    g.viz_h = 16;
    g.lineA_y = g.viz_y + g.viz_h;
    g.head_y = g.lineA_y + g.line_h;
    g.msg_y = g.H - g.line_h;
    g.lineB_y = g.msg_y - g.line_h;
    g.grid_y = g.head_y + g.line_h;
    g.rows = rows_of(a);
    g.row_h = (g.lineB_y - g.grid_y) / g.rows;
    if (g.row_h < 4) g.row_h = 4;
    g.gut_x = 3;
    g.grid_x = 32;
    g.col_w = (g.W - g.grid_x - 32) / kSteps;
    if (g.col_w < 3) g.col_w = 3;
    g.right_x = g.grid_x + kSteps * g.col_w;
    return g;
}

int draw_chip(int x, int y, const std::string& label, bool on, bool solo, pd::Color c_on,
              pd::Color c_off, pd::Color b_on, pd::Color b_off, const gfx::Font& f) {
    int w = gfx::text_cells(label) * f.w + 6;
    pd::Color border = solo ? kRed : on ? b_on : b_off;
    pd::Color fg = solo ? kRed : on ? c_on : c_off;
    gfx::frame(x, y, w, f.h, border);
    gfx::text(x + 3, y, w - 6, f, label, fg, kBg);
    return w;
}

void draw_viz(App& a, const Geom& g) {
    gfx::fill(0, g.viz_y, g.W, g.viz_h, kScreen);
    int bw = g.W / kVizBars;
    if (bw < 2) bw = 2;
    for (int i = 0; i < kVizBars; ++i) {
        float v = a.viz[(a.viz_i + i) % kVizBars];
        if (v < 0.02f) continue;
        int h = int(v * (g.viz_h - 2));
        if (h < 1) h = 1;
        gfx::fill(i * bw + 1, g.viz_y + g.viz_h - 1 - h, bw - 2, h,
                  kVizColors[std::min(4, int(v * 5))]);
    }
}

void draw_line_a(App& a, const Geom& g) {
    const gfx::Font& f = gfx::font(gfx::FONT_6x12);
    int x = 3;
    for (int i = 0; i < kPatterns; ++i) {
        std::string lbl = "P" + std::to_string(i + 1);
        x += draw_chip(x, g.lineA_y, lbl, i == a.ed.cur_pat, false, kYellow, kDim, kYellow, kLine4, f) + 3;
    }
    std::string right = std::string(a.ed.pattern_mode ? "PAT " : "SONG ") + std::to_string(a.song.tempo) + "BPM";
    gfx::text(g.W - 3 - gfx::text_cells(right) * f.w, g.lineA_y, 0, f, right, kCyan, kBg);
}

void draw_header(App& a, const Geom& g, const gfx::Font& f) {
    gfx::fill(0, g.head_y, g.W, g.line_h, kBg);
    gfx::text(g.gut_x, g.head_y, 0, f, kChannelNames[a.ed.edit_ch], kChColor[a.ed.edit_ch], kBg);
    for (int c = 0; c < kSteps; c += 4) {
        std::string n = std::to_string(c + 1);
        int x = g.grid_x + (c + 2) * g.col_w - gfx::text_cells(n) * f.w / 2;
        gfx::text(x, g.head_y, 0, f, n, kDim, kBg);
    }
    if (a.player.playing()) {
        SongPos pp = a.player.pos();
        if (pp.pat == a.ed.cur_pat) {
            int px = g.grid_x + pp.step * g.col_w;
            gfx::fill(px, g.head_y, g.col_w, g.line_h, kPlayhead);
            gfx::vline(px, g.head_y, g.line_h, kWhite);
        }
    }
}

void draw_levels(App& a, const Geom& g) {
    int grid_h = g.rows * g.row_h;
    gfx::fill(g.right_x + 1, g.grid_y, g.W - g.right_x - 1, grid_h, kBg);   // erase, then bars
    const float* lv = a.synth.ch_levels();
    for (int c = 0; c < kChannels; ++c) {
        int x = g.right_x + 2 + c * 7;
        int h = int(lv[c] * (grid_h - 4));
        if (h < 0) h = 0;
        gfx::fill(x, g.grid_y + grid_h - 2 - h, 5, h, ch_audible(a.ed, c) ? kChColor[c] : kLine0);
    }
}

void draw_grid(App& a, const Geom& g, const gfx::Font& f) {
    const EditState& ed = a.ed;
    bool drums = ed.edit_ch == CH_DRUMS;
    int grid_w = kSteps * g.col_w;
    int grid_h = g.rows * g.row_h;
    const Pattern& pat = a.song.patterns[ed.cur_pat];

    for (int r = 0; r < g.rows; ++r) {
        int y = g.grid_y + r * g.row_h;
        int midi = drums ? 0 : kChannelBase[ed.edit_ch] + (g.rows - 1 - r);
        int mc = midi % 12;
        bool black = !drums && (mc == 1 || mc == 3 || mc == 6 || mc == 8 || mc == 10);
        pd::Color rowcol = black ? kRowB : kRowA;
        gfx::fill(g.gut_x, y, g.grid_x - g.gut_x + grid_w, g.row_h, rowcol);
        std::string lbl = drums ? kDrumNames[r] : midi_to_note(midi);
        pd::Color lc = drums ? kDim : (mc == 0 ? kYellow : kDim);
        gfx::text(g.grid_x - 4 - gfx::text_cells(lbl) * f.w, y + (g.row_h - f.h) / 2, 0, f, lbl, lc, rowcol);
    }
    for (int c = 0; c <= kSteps; ++c) {
        int x = g.grid_x + c * g.col_w;
        gfx::vline(x, g.grid_y, grid_h, c % 16 == 0 ? kLine16 : c % 4 == 0 ? kLine4 : kLine0);
    }
    // No playhead column here: repainting it every step while playing costs
    // a tall fill and tears the notes. The header shows the playhead instead.
    if (drums) {
        for (int st = 0; st < kSteps; ++st) {
            uint8_t d = pat.drums[st];
            if (!d) continue;
            int x0 = g.grid_x + st * g.col_w + 1;
            int y0 = g.grid_y + (d - 1) * g.row_h + 1;
            pd::Color dc = d == DRUM_KICK ? kRed : d == DRUM_SNARE ? kYellow : kCyan;
            gfx::fill(x0, y0, g.col_w - 2, g.row_h - 2, dc);
            std::string letter = d == DRUM_KICK ? "K" : d == DRUM_SNARE ? "S" : "H";
            gfx::text(x0 + 1, y0 + (g.row_h - f.h) / 2, g.col_w - 2, f, letter, kScreen, dc);
        }
    } else {
        for (int st = 0; st < kSteps; ++st) {
            const Note& n = pat.mel[ed.edit_ch][st];
            if (!n.midi) continue;
            int row = g.rows - 1 - (n.midi - kChannelBase[ed.edit_ch]);
            if (row < 0 || row >= g.rows) continue;
            int len = std::min<int>(n.len, kSteps - st);
            int x0 = g.grid_x + st * g.col_w + 1;
            int y0 = g.grid_y + row * g.row_h + 1;
            int w = len * g.col_w - 2;
            pd::Color cc = kChColor[ed.edit_ch];
            gfx::fill(x0, y0, w, g.row_h - 2, cc);
            gfx::frame(x0, y0, w, g.row_h - 2, kScreen);
            if (w >= 3 * f.w) {
                std::string nm = midi_to_note(n.midi);
                gfx::text(x0 + 2, y0 + (g.row_h - f.h) / 2, w - 4, f, nm, kScreen, cc);
            }
        }
    }
    int cx = g.grid_x + ed.cursor_step * g.col_w;
    int cy = g.grid_y + ed.cursor_row * g.row_h;
    gfx::frame(cx, cy, g.col_w, g.row_h, kWhite);
    gfx::frame(g.grid_x, g.grid_y, grid_w, grid_h, kBorder);
    draw_levels(a, g);
}

void draw_line_b(App& a, const Geom& g, const gfx::Font& f) {
    int x = 3;
    gfx::text(x, g.lineB_y, 0, f, "ORD", kDim, kBg);
    x += 4 * f.w + 2;
    int limit = g.W - 100;
    int shown = 0;
    for (size_t i = 0; i < a.song.order.size(); ++i) {
        std::string lbl = "P" + std::to_string(a.song.order[i] + 1);
        int w = gfx::text_cells(lbl) * f.w + 6;
        if (x + w > limit) break;
        x += draw_chip(x, g.lineB_y, lbl, false, false, kCyan, kDim, kCyan, kLine4, f) + 3;
        ++shown;
    }
    if (shown < (int)a.song.order.size()) {
        std::string more = "+" + std::to_string((int)a.song.order.size() - shown);
        gfx::text(x, g.lineB_y, 0, f, more, kDim, kBg);
        x += gfx::text_cells(more) * f.w + 4;
    }
    const char* ml = "LABD";
    for (int c = 0; c < kChannels; ++c) {
        std::string lbl(1, ml[c]);
        x += draw_chip(x, g.lineB_y, lbl, a.ed.ch[c].on && !a.ed.ch[c].solo, a.ed.ch[c].solo,
                       kChColor[c], kDim, kChColor[c], kLine4, f) + 3;
    }
    std::string wave = a.ed.edit_ch == CH_DRUMS ? "----" : kWaveNames[a.ed.ch[a.ed.edit_ch].wave];
    std::string right = std::string(kChannelNames[a.ed.edit_ch]) + " " + wave + " O" +
                        std::to_string(a.ed.octave) + " L" + std::to_string(a.ed.note_len);
    gfx::text(g.W - 3 - gfx::text_cells(right) * f.w, g.lineB_y, 0, f, right, kChColor[a.ed.edit_ch], kBg);
}

// Cheap per-frame update while playing: only the animated strips, no full
// clear (a full-screen clear is ~200 KB over SPI - it flashes and starves
// the audio pump). Full draw() runs only when the state actually changed.
void draw_overlay(App& a) {
    Geom g = geom_of(a);
    draw_viz(a, g);
    draw_header(a, g, gfx::font(gfx::FONT_6x12));
    draw_levels(a, g);
}

void draw(App& a) {
    const ui::Theme& th = a.ui.theme();
    const gfx::Font& f = gfx::font(gfx::FONT_6x12);
    Geom g = geom_of(a);
    gfx::clear(kBg);
    std::string left;
    if (a.toast_until && !pd::sys::reached(a.toast_until))
        left = a.toast;
    else
        left = std::string("POD TUNES \xC2\xB7 ") + a.song.name;
    a.bar.draw(th, gfx::clip_cells(left, 44), "Esc menu", true);
    draw_viz(a, g);
    draw_line_a(a, g);
    draw_header(a, g, f);
    draw_grid(a, g, f);
    draw_line_b(a, g, f);
    gfx::text(3, g.msg_y, g.W - 6, f, kHint, th.dim, kBg);
}

// ---------------------------------------------------------------- playback
void toggle_play(App& a) {
    if (a.player.playing())
        a.player.stop();
    else
        a.player.play(0);
    a.dirty = true;
}

void play_from_cursor(App& a) {
    long start;
    if (a.ed.pattern_mode || a.song.order.empty()) {
        start = a.ed.cursor_step;
    } else {
        long i = 0;
        for (size_t k = 0; k < a.song.order.size(); ++k)
            if (a.song.order[k] == a.ed.cur_pat) { i = long(k); break; }
        start = i * kSteps + a.ed.cursor_step;
    }
    a.player.play(start);
    a.dirty = true;
}

// ---------------------------------------------------------------- editing
void place_note(App& a, int midi) {
    Pattern& pat = a.song.patterns[a.ed.cur_pat];
    pat.mel[a.ed.edit_ch][a.ed.cursor_step] = {uint8_t(midi), uint8_t(a.ed.note_len)};
    a.synth.audition(a.ed.edit_ch, uint8_t(midi), a.ed.ch[a.ed.edit_ch].wave);
    a.ed.cursor_step = std::min(kSteps - 1, a.ed.cursor_step + 1);
}

void drum_key(App& a, int drum) {
    Pattern& pat = a.song.patterns[a.ed.cur_pat];
    if (pat.drums[a.ed.cursor_step] == drum)
        pat.drums[a.ed.cursor_step] = DRUM_NONE;
    else {
        pat.drums[a.ed.cursor_step] = uint8_t(drum);
        a.synth.audition_drum(drum);
    }
    a.ed.cursor_step = std::min(kSteps - 1, a.ed.cursor_step + 1);
}

void clear_cell(App& a) {
    Pattern& pat = a.song.patterns[a.ed.cur_pat];
    if (a.ed.edit_ch == CH_DRUMS)
        pat.drums[a.ed.cursor_step] = DRUM_NONE;
    else
        pat.mel[a.ed.edit_ch][a.ed.cursor_step] = Note{};
}

void pat_step(App& a, int d) {
    a.ed.cur_pat = (a.ed.cur_pat + d + kPatterns) % kPatterns;
    a.ed.cursor_step = 0;
    a.ed.cursor_row = std::min(a.ed.cursor_row, rows_of(a) - 1);
    a.dirty = true;
    toast(a, "PATTERN P" + std::to_string(a.ed.cur_pat + 1));
}

bool pattern_empty(const Song& s, int p) {
    const Pattern& pat = s.patterns[p];
    for (int c = 0; c < 3; ++c)
        for (int st = 0; st < kSteps; ++st)
            if (pat.mel[c][st].midi) return false;
    for (int st = 0; st < kSteps; ++st)
        if (pat.drums[st]) return false;
    return true;
}

void copy_pattern(App& a) {
    int target = -1;
    for (int i = 0; i < kPatterns; ++i) {
        int idx = (a.ed.cur_pat + 1 + i) % kPatterns;
        if (pattern_empty(a.song, idx)) { target = idx; break; }
    }
    if (target < 0) target = (a.ed.cur_pat + 1) % kPatterns;
    a.song.patterns[target] = a.song.patterns[a.ed.cur_pat];
    a.ed.cur_pat = target;
    a.ed.cursor_step = 0;
    toast(a, "PATTERN COPIED TO P" + std::to_string(target + 1));
    touch(a);
}

void clear_pattern(App& a) {
    if (!ui::confirm(a.ui, "Clear pattern", "Clear pattern P" + std::to_string(a.ed.cur_pat + 1) + "?")) return;
    a.song.patterns[a.ed.cur_pat].clear();
    toast(a, "PATTERN P" + std::to_string(a.ed.cur_pat + 1) + " CLEARED");
    touch(a);
}

void cycle_track(App& a, int d) {
    a.ed.edit_ch = (a.ed.edit_ch + d + kChannels) % kChannels;
    a.ed.octave = a.ed.edit_ch == CH_BASS ? 2 : 4;
    a.ed.cursor_row = std::min(a.ed.cursor_row, rows_of(a) - 1);
    a.dirty = true;
    toast(a, std::string("TRACK ") + kChannelNames[a.ed.edit_ch]);
}

void cycle_wave(App& a) {
    if (a.ed.edit_ch == CH_DRUMS) return;
    int w = (a.ed.ch[a.ed.edit_ch].wave + 1) % 4;
    a.ed.ch[a.ed.edit_ch].wave = w;
    a.dirty = true;
    toast(a, std::string("WAVE ") + kWaveNames[w]);
}

void tempo_key(App& a, int d) {
    a.song.tempo = std::max(60, std::min(240, a.song.tempo + d));
    touch(a);
}

// ---------------------------------------------------------------- dialogs
void export_json_ui(App& a) {
    std::string path = "/" + slug(a.song.name) + ".json";
    a.ui.kbd.flush();
    if (!ui::prompt(a.ui, "Export JSON", path, "path on the SD card", false, 200)) return;
    if (!pd::fs::write_file(path.c_str(), song_to_json(a.song, true))) {
        ui::message(a.ui, "Export JSON", {"Write failed:", path});
        return;
    }
    toast(a, "JSON SAVED: " + path);
}

void import_json_ui(App& a) {
    std::string dir = "/";
    std::string file;
    ui::BrowseOptions opt;
    opt.filter = ui::extension_filter(".json");
    opt.empty_hint = "No .json songs on the card";
    a.ui.kbd.flush();
    if (!ui::browse(a.ui, "Import song", dir, file, opt)) return;
    std::string text;
    if (!pd::fs::read_file(file.c_str(), text, 1u << 20) || !song_from_json(text, a.song)) {
        ui::message(a.ui, "Import song", {"Invalid song file:", file});
        return;
    }
    // song_from_json only writes its output on success, so a.song is intact above
    a.ed.cur_pat = 0;
    a.ed.cursor_step = 0;
    a.ed.cursor_row = std::min(a.ed.cursor_row, rows_of(a) - 1);
    save_song(a);
    a.song_dirty = false;
    a.dirty = true;
    toast(a, "SONG LOADED: " + a.song.name);
}

void export_wav_ui(App& a) {
    if (a.player.playing()) a.player.stop();
    std::string path = "/" + slug(a.song.name) + ".wav";
    a.ui.kbd.flush();
    if (!ui::prompt(a.ui, "Export WAV", path, "path on the SD card", false, 200)) return;
    long total = (a.ed.pattern_mode || a.song.order.empty()) ? long(kSteps) * 4
                                                             : long(a.song.order.size()) * kSteps;
    auto progress = [&](long done, long) -> bool {
        pump(a);   // keep the live output fed (silence) while rendering
        ui::progress(a.ui, "Rendering WAV",
                     std::to_string(done) + " / " + std::to_string(total) + " steps", uint32_t(done),
                     uint32_t(total));
        return !a.ui.kbd.esc_pressed();
    };
    bool ok = export_wav(a.song, a.ed, path, progress);
    a.dirty = true;
    toast(a, ok ? "WAV SAVED: " + path : "WAV EXPORT CANCELLED");
}

void reset_demo(App& a) {
    if (!ui::confirm(a.ui, "Reset", "Reset to the demo song? Your changes are lost.")) return;
    build_demo(a.song);
    a.ed.cur_pat = 0;
    a.ed.cursor_step = 0;
    save_song(a);
    a.song_dirty = false;
    a.dirty = true;
    toast(a, "DEMO SONG RESTORED");
}

void song_name_ui(App& a) {
    std::string name = a.song.name;
    a.ui.kbd.flush();
    if (!ui::prompt(a.ui, "Song name", name, "up to 24 characters", false, 24)) return;
    a.song.name = name.empty() ? "UNTITLED" : name;
    touch(a);
    toast(a, "SONG: " + a.song.name);
}

void help_ui(App& a) {
    std::vector<std::string> lines = {
        std::string("Pod Tunes ") + PD_APP_VERSION + " - 8-bit music creator",
        "",
        "Space / F2    play, stop",
        "Enter / F3    play from the cursor",
        "F4            song / pattern mode",
        "[ ] or F5/F6  previous / next pattern",
        "F7 / F8       copy / clear pattern",
        "Tab / F9      next track (LEAD ARP BASS DRM)",
        "F10           next waveform (SQ25 SQ50 SQ12 TRI)",
        "- =           tempo down / up",
        ", .           octave down / up",
        "Arrows        move the cursor (Shift: 4 steps)",
        "Backspace     clear the cell",
        "Z S X D C V G B H N J M   notes, lower octave",
        "Q 2 W 3 E R 5 T 6 Y 7 U   notes, upper octave",
        "I 9 O 0 P                 two octaves up",
        "on the DRM track: Z kick, X snare, C hat",
        "Ctrl+E        export the song as JSON",
        "Ctrl+I        import a song (JSON)",
        "Ctrl+W        render the song to a .wav file",
        "Ctrl+R        reset to the demo song",
        "Esc           menu: order, mute/solo, volumes, theme...",
    };
    ui::text_view(a.ui, "Pod Tunes help", lines);
}

void about_ui(App& a) {
    ui::message(a.ui, "About", {
        std::string("Pod Tunes ") + PD_APP_VERSION,
        "8-bit music creator: 4 channels, 8 patterns of 32 steps,",
        "song order, JSON songs, WAV export.",
        std::string("Board: ") + pd::board::name() + " (" + pd::board::chip() + ")",
        std::string("Terminal: ") + a.term.name(),
        a.audio ? "Audio: PWM, " + std::to_string(kSampleRate) + " Hz"
                : std::string("Audio: not available"),
    });
}

// ---------------------------------------------------------------- menu
void main_menu(App& a) {
    a.ui.kbd.flush();
    int mode_idx = a.ed.pattern_mode ? 1 : 0;
    int wave_idx = a.ed.ch[a.ed.edit_ch].wave;
    int len_idx = 0;
    while (len_idx < 3 && (1 << len_idx) != a.ed.note_len) ++len_idx;
    const char* pat_lbl[kPatterns] = {"P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8"};
    std::string mute_hint[kChannels];
    for (int c = 0; c < kChannels; ++c)
        mute_hint[c] = a.ed.ch[c].solo ? "SOLO" : a.ed.ch[c].on ? "on" : "off";

    enum {
        M_RESUME, M_PLAY, M_FROM, M_MODE, M_PATP, M_PATP2, M_JUMP, M_COPY, M_CLEAR,
        M_TRACK, M_WAVE, M_OCT, M_LEN, M_MUTE0, M_MUTE1, M_MUTE2, M_MUTE3,
        M_VOL0, M_VOL1, M_VOL2, M_VOL3, M_TEMPO, M_NAME, M_EXP, M_IMP, M_WAV, M_RESET,
        M_THEME, M_MVOL, M_HELP, M_ABOUT, M_REBOOT, M_COUNT
    };
    static ui::MenuItem items[M_COUNT];   // static: 32 x 72 B, too big for the 2 KB stack
    items[M_RESUME] = ui::action("Resume", "Esc");
    items[M_PLAY] = ui::action(a.player.playing() ? "Stop" : "Play", "Space");
    items[M_FROM] = ui::action("Play from cursor", "Enter");
    items[M_MODE] = ui::choice("Mode", &mode_idx, kModeNames, 2);
    items[M_PATP] = ui::action("Pattern -", "[ / F5");
    items[M_PATP2] = ui::action("Pattern +", "] / F6");
    items[M_JUMP] = ui::choice("Jump to pattern", &a.ed.cur_pat, pat_lbl, kPatterns);
    items[M_COPY] = ui::action("Copy pattern", "F7");
    items[M_CLEAR] = ui::action("Clear pattern", "F8");
    items[M_TRACK] = ui::choice("Track", &a.ed.edit_ch, kChannelNames, kChannels);
    items[M_WAVE] = ui::choice("Wave", &wave_idx, kWaveNames, 4);
    items[M_OCT] = ui::range("Octave", &a.ed.octave, 1, 7, 1, "%d");
    items[M_LEN] = ui::choice("Note length", &len_idx, kLenNames, 4);
    items[M_MUTE0] = ui::action("Mute/solo LEAD", mute_hint[0].c_str());
    items[M_MUTE1] = ui::action("Mute/solo ARP", mute_hint[1].c_str());
    items[M_MUTE2] = ui::action("Mute/solo BASS", mute_hint[2].c_str());
    items[M_MUTE3] = ui::action("Mute/solo DRM", mute_hint[3].c_str());
    items[M_VOL0] = ui::range("Volume LEAD", &a.ed.ch[0].vol, 0, 100, 5, "%d%%");
    items[M_VOL1] = ui::range("Volume ARP", &a.ed.ch[1].vol, 0, 100, 5, "%d%%");
    items[M_VOL2] = ui::range("Volume BASS", &a.ed.ch[2].vol, 0, 100, 5, "%d%%");
    items[M_VOL3] = ui::range("Volume DRM", &a.ed.ch[3].vol, 0, 100, 5, "%d%%");
    items[M_TEMPO] = ui::range("Tempo", &a.song.tempo, 60, 240, 1, "%d BPM");
    items[M_NAME] = ui::action("Song name...");
    items[M_EXP] = ui::action("Export JSON", "Ctrl+E");
    items[M_IMP] = ui::action("Import JSON", "Ctrl+I");
    items[M_WAV] = ui::action("Export WAV", "Ctrl+W");
    items[M_RESET] = ui::action("Reset to demo song", "Ctrl+R");
    items[M_THEME] = ui::choice("Theme", &a.set.theme, kThemeNames, 3);
    items[M_MVOL] = ui::range("Master volume", &a.set.volume, 0, 100, 5, "%d%%");
    items[M_HELP] = ui::action("Help", "F1");
    items[M_ABOUT] = ui::action("About");
    items[M_REBOOT] = ui::action("Save and reboot");
    ui::MenuOptions opt;
    opt.footer = "Pod Tunes " PD_APP_VERSION;
    opt.on_change = [&](int) {
        a.ed.pattern_mode = mode_idx == 1;
        a.ed.ch[a.ed.edit_ch].wave = wave_idx;
        a.ed.note_len = 1 << len_idx;
        a.ui.th = &ui::theme(a.set.theme);
        if (a.audio) a.audio->set_volume(a.set.volume);
        a.dirty = true;
    };
    const int r = ui::menu(a.ui, "POD TUNES", items, opt);
    if (r >= 0) {
        switch (r) {
            case M_PLAY: toggle_play(a); break;
            case M_FROM: play_from_cursor(a); break;
            case M_PATP: pat_step(a, -1); break;
            case M_PATP2: pat_step(a, +1); break;
            case M_COPY: copy_pattern(a); break;
            case M_CLEAR: clear_pattern(a); break;
            case M_MUTE0: case M_MUTE1: case M_MUTE2: case M_MUTE3: {
                int c = r - M_MUTE0;
                ChanState& st = a.ed.ch[c];
                if (st.solo) { st.solo = false; st.on = false; }
                else if (st.on) { st.on = false; st.solo = true; for (int x = 0; x < kChannels; ++x) if (x != c) a.ed.ch[x].solo = false; }
                else { st.on = true; st.solo = false; }
                touch(a);
                break;
            }
            case M_TEMPO: touch(a); break;
            case M_NAME: song_name_ui(a); break;
            case M_EXP: export_json_ui(a); break;
            case M_IMP: import_json_ui(a); break;
            case M_WAV: export_wav_ui(a); break;
            case M_RESET: reset_demo(a); break;
            case M_HELP: help_ui(a); break;
            case M_ABOUT: about_ui(a); break;
            case M_REBOOT:
                save_song(a);
                a.save_settings();
                pd::sys::reboot();
                break;
            default: break;
        }
        if (r == M_TRACK) {
            a.ed.octave = a.ed.edit_ch == CH_BASS ? 2 : 4;
            a.ed.cursor_row = std::min(a.ed.cursor_row, rows_of(a) - 1);
        }
    }
    a.save_settings();
    a.dirty = true;
}

// ---------------------------------------------------------------- keys
void handle_key(App& a, const pd::input::KeyEvent& ev) {
    EditState& ed = a.ed;
    if (ev.code == key::Esc) { main_menu(a); return; }
    if (ev.code == key::F1) { help_ui(a); a.dirty = true; return; }
    if (ev.is_ctrl('e')) { export_json_ui(a); a.dirty = true; return; }
    if (ev.is_ctrl('i')) { import_json_ui(a); a.dirty = true; return; }
    if (ev.is_ctrl('w')) { export_wav_ui(a); a.dirty = true; return; }
    if (ev.is_ctrl('r')) { reset_demo(a); a.dirty = true; return; }
    if (ev.code == key::F2 || ev.code == key::Space) { toggle_play(a); return; }
    if (ev.code == key::F3 || ev.code == key::Enter) { play_from_cursor(a); return; }
    if (ev.code == key::F4) {
        ed.pattern_mode = !ed.pattern_mode;
        a.dirty = true;
        toast(a, ed.pattern_mode ? "LOOPING PATTERN P" + std::to_string(ed.cur_pat + 1) : "PLAYING SONG ORDER");
        return;
    }
    if (ev.code == key::F5 || ev.code == '[') { pat_step(a, -1); touch(a); return; }
    if (ev.code == key::F6 || ev.code == ']') { pat_step(a, +1); touch(a); return; }
    if (ev.code == key::F7) { copy_pattern(a); return; }
    if (ev.code == key::F8) { clear_pattern(a); return; }
    if (ev.code == key::F9 || ev.code == key::Tab) { cycle_track(a, 1); return; }
    if (ev.code == key::F10) { cycle_wave(a); return; }
    if (ev.code == '-') { tempo_key(a, -2); return; }
    if (ev.code == '=' || ev.code == '+') { tempo_key(a, +2); return; }
    if (ev.code == ',') { ed.octave = std::max(1, ed.octave - 1); a.dirty = true; return; }
    if (ev.code == '.') { ed.octave = std::min(7, ed.octave + 1); a.dirty = true; return; }
    if (ev.code == key::Left) { ed.cursor_step = std::max(0, ed.cursor_step - (ev.shift ? 4 : 1)); a.dirty = true; return; }
    if (ev.code == key::Right) { ed.cursor_step = std::min(kSteps - 1, ed.cursor_step + (ev.shift ? 4 : 1)); a.dirty = true; return; }
    if (ev.code == key::Up) { ed.cursor_row = std::max(0, ed.cursor_row - 1); a.dirty = true; return; }
    if (ev.code == key::Down) { ed.cursor_row = std::min(rows_of(a) - 1, ed.cursor_row + 1); a.dirty = true; return; }
    if (ev.code == key::Backspace || ev.code == key::Del) { clear_cell(a); touch(a); return; }
    if (ev.repeat) return;
    char c = key::lower(ev.code);
    if (ed.edit_ch == CH_DRUMS) {
        int d = c == 'z' ? DRUM_KICK : c == 'x' ? DRUM_SNARE : c == 'c' ? DRUM_HAT : 0;
        if (d) { drum_key(a, d); touch(a); }
        return;
    }
    int off = keymap_offset(c);
    if (off >= 0) {
        int midi = (ed.octave + 1) * 12 + off;
        place_note(a, std::min(127, midi));
        touch(a);
    }
}

}  // namespace

int main() {
    pd::diag::begin("Pod Tunes", PD_APP_VERSION, kStages, pd::array_count(kStages));
    pd::diag::stage(kStart);

    pd::TerminalConfig cfg;
    cfg.clock_khz = pd::board::fast_clock_khz();
    cfg.keyboard_on_core1 = true;   // keyboard reads never stall the audio
    pd::Terminal& term = pd::terminal_init(cfg);
    pd::gfx::init(term.display());
    pd::diag::display_ready();
    pd::diag::stage(kDisplay);

    pd::ui::Context ui(term.keyboard(), pd::ui::THEME_NIGHT);
    static App a(term, ui);   // static: App is ~3.4 KB, the device main stack is 2 KB

    pd::diag::stage(kSd);
    pd::app::mount_storage(ui);
    pd::fs::mkdirs(kWorkDir);

    pd::diag::stage(kSettings);
    a.load_settings();
    ui.th = &pd::ui::theme(a.set.theme);
    std::string text;
    bool loaded = pd::fs::read_file(kSongPath, text) && song_from_json(text, a.song);
    if (!loaded) {
        build_demo(a.song);
        save_song(a);
    }

    a.audio = term.audio();
    a.synth.start(kSampleRate);
    if (a.audio) {
        a.audio->start(kSampleRate);
        a.audio->set_volume(a.set.volume);
    }
    a.player.attach(&a.song, &a.ed, &a.synth);
    ui.idle = []() { pump(a); };   // a is static: no capture needed

    pd::diag::stage(kReady);
    pd::diag::done();
    toast(a, loaded ? "WELCOME BACK" : "DEMO SONG LOADED - ESC FOR MENU");

    uint32_t last_overlay = 0;
    for (;;) {
        pump(a);   // audio first: keep the PWM buffer fed before any drawing
        pd::input::KeyEvent ev;
        while (ui.kbd.poll(ev)) handle_key(a, ev);
        if (a.song_dirty && pd::sys::reached(a.last_edit_ms + 400)) {
            save_song(a);
            a.song_dirty = false;
        }
        if (a.toast_until && pd::sys::reached(a.toast_until)) {
            a.toast_until = 0;
            a.dirty = true;
        }
        if (a.dirty) {
            draw(a);
            last_overlay = pd::sys::now_ms();
            a.dirty = false;
        } else if (a.player.playing() && pd::sys::reached(last_overlay + 100)) {
            draw_overlay(a);   // small partial redraw, ~10 Hz while playing
            last_overlay = pd::sys::now_ms();
        }
        pd::sys::sleep_ms(2);
    }
    return 0;
}
