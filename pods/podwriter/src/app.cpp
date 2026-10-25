#include "app.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include "diag.h"
#include "pd/diag.h"

namespace {
const char* const kWorkDir = "/.fwedit";
const char* const kClipPath = "/.fwedit/clip.txt";
const char* const kSettingsPath = "/.fwedit/settings.txt";
const char* const kTab = "    ";

uint32_t nowMs() { return pd::sys::now_ms(); }

// Battery in the south bridge's format: percent, bit 7 = charging, -1 = unknown.
int batteryRaw() {
    pd::hal::Power* p = pd::terminal().power();
    if (!p) return -1;
    const int b = p->battery_percent();
    if (b < 0) return -1;
    return b | (p->charging() ? 0x80 : 0);
}
bool isSpace(uint8_t c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; }
uint8_t g_chunk[4096];
}  // namespace

App::App(Keyboard& kbd) : kbd_(kbd), ui_(kbd, pd::ui::THEME_NIGHT), lay_(tb_) { invalidate(); }

// ---------------------------------------------------------------------------
// Setup and settings
// ---------------------------------------------------------------------------

void App::waitForCard() {
    bool shown = false;
    while (!fs::mount()) {
        if (!shown) {
            const Theme& t = th();
            pd::gfx::fill(0, 0, pd::gfx::width(), pd::gfx::height(), t.bg);
            pd::gfx::text(40, 140, 240, pd::gfx::font(1), "Insert an SD card", t.fg, t.bg);
            pd::gfx::text(40, 160, 240, pd::gfx::font(0), "(FAT32 or exFAT)", t.dim, t.bg);
            shown = true;
        }
        pd::sys::sleep_ms(1000);
    }
}

void App::loadSettings() {
    fs::File f;
    if (!f.open(kSettingsPath, fs::Mode::Read)) return;
    static char buf[2048];
    const int n = f.read(buf, sizeof buf - 1);
    if (n <= 0) return;
    buf[n] = 0;
    char* save = nullptr;
    for (char* ln = strtok_r(buf, "\n", &save); ln; ln = strtok_r(nullptr, "\n", &save)) {
        char* eq = strchr(ln, '=');
        if (!eq) continue;
        *eq = 0;
        const char* v = eq + 1;
        if (!strcmp(ln, "font")) set_.font = std::max(0, std::min(pd::gfx::FONT_COUNT - 1, atoi(v)));
        else if (!strcmp(ln, "theme")) set_.theme = std::max(0, std::min(pd::ui::kThemeCount - 1, atoi(v)));
        else if (!strcmp(ln, "status")) set_.status = atoi(v) != 0;
        else if (!strcmp(ln, "syntax")) set_.syntax = atoi(v) != 0;
        else if (!strcmp(ln, "file")) set_.file = v;
        else if (!strcmp(ln, "cwd")) set_.cwd = v;
        else if (!strcmp(ln, "pos")) set_.pos = uint32_t(strtoul(v, nullptr, 10));
        else if (!strcmp(ln, "author")) set_.author = v;
        else if (!strcmp(ln, "lang")) set_.lang = v;
        else if (!strcmp(ln, "x3_url")) set_.x3Url = v;
        else if (!strcmp(ln, "x3_dir")) set_.x3Dir = v;
        else if (!strcmp(ln, "x3_ip")) set_.x3Ip = v;
    }
    ui_.th = &pd::ui::theme(set_.theme);
}

void App::saveSettings() {
    set_.file = tb_.path();
    set_.pos = cursor_;
    static char buf[2048];
    const int n = snprintf(buf, sizeof buf,
                           "font=%d\ntheme=%d\nstatus=%d\nsyntax=%d\nfile=%s\ncwd=%s\npos=%lu\n"
                           "author=%s\nlang=%s\nx3_url=%s\nx3_dir=%s\nx3_ip=%s\n",
                           set_.font, set_.theme, set_.status ? 1 : 0, set_.syntax ? 1 : 0, set_.file.c_str(),
                           set_.cwd.c_str(), (unsigned long)set_.pos, set_.author.c_str(),
                           set_.lang.c_str(), set_.x3Url.c_str(), set_.x3Dir.c_str(), set_.x3Ip.c_str());
    fs::File f;
    if (f.open(kSettingsPath, fs::Mode::Write)) f.write(buf, uint32_t(std::min<int>(n, sizeof buf)));
}

// ---------------------------------------------------------------------------
// Geometry and rendering
// ---------------------------------------------------------------------------

void App::invalidate() {
    for (uint32_t& h : rowHash_) h = 0;
    statusCache_.clear();
}

void App::relayout() {
    ui_.th = &pd::ui::theme(set_.theme);
    font_ = &pd::gfx::font(set_.font);
    const int statusH = set_.status ? kStatusH : 0;
    cols_ = pd::gfx::width() / font_->w;
    margin_ = cols_ >= 40 ? 2 : 1;
    textW_ = cols_ - 2 * margin_;
    lay_.setWidth(textW_);
    rows_ = std::min(kMaxRows, (pd::gfx::height() - statusH) / font_->h);
    x0_ = (pd::gfx::width() - cols_ * font_->w) / 2 + margin_ * font_->w;
    y0_ = ((pd::gfx::height() - statusH) - rows_ * font_->h) / 2;
    pd::gfx::fill(0, 0, pd::gfx::width(), pd::gfx::height() - statusH, th().bg);
    if (statusH) pd::gfx::fill(0, pd::gfx::height() - statusH, pd::gfx::width(), statusH, th().status_bg);
    invalidate();
    normalizeTop();
    ensureVisible();
}

syn::Lang App::docLang() const {
    return set_.syntax ? syn::langForPath(tb_.path().c_str()) : syn::None;
}

void App::synFeed(uint32_t from, uint32_t to) {
    static uint8_t buf[512 + syn::kLookahead];
    while (from < to) {
        const uint32_t n = std::min<uint32_t>(to - from, 512);
        const uint32_t got = tb_.read(from, buf, n + syn::kLookahead);
        if (got < n) break;  // read error: leave the colours approximate
        lex_.run(buf, n, got, nullptr);
        from += n;
    }
}

// Brings lex_ to the state at top_. The state is cached; it stays valid while
// edits happen after it (plus the lookahead it may have peeked at), and
// scrolling down continues from it instead of starting over.
void App::synPrepare(syn::Lang lang) {
    const uint32_t dirty = tb_.takeDirtyFrom();
    if (synPos_ != kNone &&
        (lang != synLang_ || uint64_t(dirty) < uint64_t(synPos_) + synState_.rem + syn::kLookahead))
        synPos_ = kNone;
    if (synPos_ != kNone && top_ == synPos_) {
        lex_.setState(synState_);
        return;
    }
    if (synPos_ != kNone && top_ > synPos_ && top_ - synPos_ <= syn::kBackScan) {
        lex_.setState(synState_);
        synFeed(synPos_, top_);
    } else {
        // Re-lex from the first line start inside the back-scan window.
        uint32_t start = 0;
        if (top_ > syn::kBackScan) {
            start = top_ - syn::kBackScan;
            static uint8_t buf[512];
            for (uint32_t p = start; p < top_;) {
                const uint32_t n = tb_.read(p, buf, std::min<uint32_t>(top_ - p, sizeof buf));
                if (!n) break;
                const void* nl = memchr(buf, '\n', n);
                if (nl) {
                    start = p + uint32_t(static_cast<const uint8_t*>(nl) - buf) + 1;
                    break;
                }
                p += n;
            }
        }
        lex_.reset(lang);
        synFeed(start, top_);
    }
    synState_ = lex_.state();
    synPos_ = top_;
    synLang_ = lang;
}

void App::render() {
    const Theme& t = th();
    const uint32_t size = tb_.size();
    const Row crow = lay_.rowContaining(cursor_);
    const int ccol = lay_.colOf(crow, cursor_);
    uint32_t selA = 0, selB = 0;
    const bool sel = hasSel();
    if (sel) selRange(&selA, &selB);
    const syn::Lang lang = docLang();
    const bool hl = lang != syn::None;
    if (hl) synPrepare(lang);

    static Cell cells[64];
    static uint8_t bytes[400 + syn::kLookahead];
    static uint8_t cls[400];
    constexpr uint32_t kRowMax = sizeof cls;
    uint32_t s = top_;
    bool ended = false;
    for (int r = 0; r < rows_; ++r) {
        int n = 0;
        int curCol = -1;
        if (!ended) {
            const Row row = lay_.row(s);
            if (row.start == crow.start) curCol = ccol;
            const uint32_t rowLen = std::min<uint32_t>(row.next - row.start, kRowMax);
            const uint32_t got =
                tb_.read(row.start, bytes, rowLen + (hl ? syn::kLookahead : 0));
            const uint32_t len = std::min(got, rowLen);
            if (hl) {
                lex_.run(bytes, len, got, cls);
                if (row.next - row.start > len) synFeed(row.start + len, row.next);
            }
            uint32_t i = 0;
            while (i < len && n < 64) {
                const uint8_t c = bytes[i];
                if (c == '\n') break;
                if (c == '\r' || Layout::isCont(c)) { ++i; continue; }
                int used = 1;
                uint32_t cp = pd::gfx::decode_utf8(bytes + i, len - i, &used);
                if (cp == '\t') cp = ' ';
                else if (cp < 0x20 || cp > 0xFFFF) cp = '?';
                const uint32_t pos = row.start + i;
                const bool inSel = sel && pos >= selA && pos < selB;
                const pd::Color fg = (hl && cls[i] != syn::Plain) ? pd::syn::palette(set_.theme)[cls[i]] : t.fg;
                cells[n++] = Cell{uint16_t(cp), fg, inSel ? t.sel : t.bg};
                i += uint32_t(used);
            }
            // Show selected line breaks as one highlighted blank cell.
            if (sel && row.nl && n < 64 && row.next - 1 >= selA && row.next - 1 < selB)
                cells[n++] = Cell{' ', t.fg, t.sel};
            if (row.next == row.start || (row.next == size && !row.nl)) ended = true;
            else s = row.next;
        }
        uint32_t h = 2166136261u ^ uint32_t(n) ^ (uint32_t(curCol + 2) << 20);
        for (int k = 0; k < n; ++k) {
            h = (h ^ (cells[k].cp | (uint32_t(cells[k].bg) << 16))) * 16777619u;
            h = (h ^ cells[k].fg) * 16777619u;
        }
        if (h == 0) h = 1;
        if (h != rowHash_[r]) {
            pd::gfx::draw_cells(x0_, y0_ + r * font_->h, pd::gfx::width() - x0_, *font_, cells, n, t.bg, curCol,
                           t.cursor);
            rowHash_[r] = h;
        }
    }
    renderStatus(statusOverlay_);
}

void App::renderStatus(bool force) {
    const bool msgActive = !msg_.empty();
    if (!set_.status && !msgActive) {
        if (statusOverlay_) {  // overlay message expired: repaint text rows
            statusOverlay_ = false;
            pd::gfx::fill(0, pd::gfx::height() - kStatusH, pd::gfx::width(), kStatusH, th().bg);
            invalidate();
            render();
        }
        return;
    }
    if (!set_.status) statusOverlay_ = true;
    const Theme& t = th();
    const Font& f = pd::gfx::font(0);
    const int cells = (pd::gfx::width() - 8) / f.w;
    std::string left = msgActive ? msg_ : docName() + (tb_.modified() ? " *" : "");
    char right[64];
    const uint32_t size = tb_.size();
    const unsigned pct = size ? unsigned(uint64_t(cursor_) * 100 / size) : 100;
    char bat[12] = "";
    if (battery_ >= 0) snprintf(bat, sizeof bat, "  %s%d%%", (battery_ & 0x80) ? "+" : "", battery_ & 0x7F);
    const char* lang = syn::langName(docLang());
    snprintf(right, sizeof right, "%s%s%s%s%3u%%%s", net::connected() ? "WiFi  " : "", lang,
             *lang ? "  " : "", overwrite_ ? "OVR  " : "", pct, bat);
    const int rc = int(strlen(right));
    std::string line = left;
    int lc = pd::gfx::text_cells(line);
    while (lc > cells - rc - 1) {  // trim from the end, UTF-8 safe
        line.pop_back();
        while (!line.empty() && (uint8_t(line.back()) & 0xC0) == 0x80) line.pop_back();
        if (!line.empty() && uint8_t(line.back()) >= 0xC0) line.pop_back();
        lc = pd::gfx::text_cells(line);
    }
    line.append(size_t(cells - rc - lc), ' ');
    line += right;
    if (!force && line == statusCache_) return;
    statusCache_ = line;
    pd::gfx::text(4, pd::gfx::height() - kStatusH + 1, pd::gfx::width() - 8, f, line, msgActive ? t.accent : t.status_fg,
                  t.status_bg);
}

void App::setMessage(const std::string& m, uint32_t ms) {
    msg_ = m;
    msgUntil_ = nowMs() + ms;
    statusCache_.clear();
}

void App::showProgress(const char* what, uint32_t done, uint32_t total) {
    char b[64];
    snprintf(b, sizeof b, "%s %u%%", what, total ? unsigned(uint64_t(done) * 100 / total) : 100);
    msg_ = b;
    msgUntil_ = nowMs() + 60000;
    renderStatus(true);
}

TextBuffer::Progress App::progressFn(const char* what, bool cancellable) {
    uint32_t next = nowMs() + 300;
    return [this, what, cancellable, next](uint32_t done, uint32_t total) mutable -> bool {
        const uint32_t t = nowMs();
        if (int32_t(t - next) < 0) return true;
        next = t + 300;
        showProgress(what, done, total);
        if (cancellable) {
            KeyEvent ev;
            if (kbd_.poll(ev) && ev.code == key::Esc) return false;
        }
        return true;
    };
}

// ---------------------------------------------------------------------------
// Cursor and view
// ---------------------------------------------------------------------------

void App::selRange(uint32_t* a, uint32_t* b) const {
    *a = std::min(anchor_, cursor_);
    *b = std::max(anchor_, cursor_);
}

void App::moveTo(uint32_t pos, bool extend) {
    if (extend) {
        if (anchor_ == kNone) anchor_ = cursor_;
    } else {
        anchor_ = kNone;
    }
    cursor_ = std::min(pos, tb_.size());
    tb_.breakUndoMerge();
    ensureVisible();
}

uint32_t App::vertical(uint32_t pos, int dir) {
    const uint32_t size = tb_.size();
    const Row r = lay_.rowContaining(pos);
    if (goal_ < 0) goal_ = lay_.colOf(r, pos);
    if (dir < 0) {
        if (r.start == 0) return 0;
        return lay_.posAtCol(lay_.row(lay_.prevRowStart(r.start)), goal_);
    }
    if (r.next == r.start || (r.next == size && !r.nl)) return size;
    return lay_.posAtCol(lay_.row(r.next), goal_);
}

void App::normalizeTop() {
    if (top_ > tb_.size()) top_ = tb_.size();
    top_ = lay_.rowContaining(top_).start;
}

void App::ensureVisible() {
    const uint32_t size = tb_.size();
    const Row crow = lay_.rowContaining(cursor_);
    if (crow.start < top_) {
        top_ = crow.start;
        return;
    }
    uint32_t s = top_;
    for (int i = 0; i < rows_; ++i) {
        if (s == crow.start) return;
        const Row r = lay_.row(s);
        if (r.next == r.start || (r.next == size && !r.nl)) break;
        s = r.next;
    }
    uint32_t t = crow.start;
    for (int k = 0; k < rows_ - 1 && t > 0; ++k) t = lay_.prevRowStart(t);
    top_ = t;
}

void App::centerCursor() {
    uint32_t t = lay_.rowContaining(cursor_).start;
    for (int k = 0; k < rows_ / 3 && t > 0; ++k) t = lay_.prevRowStart(t);
    top_ = t;
}

uint32_t App::wordLeft(uint32_t p) {
    const uint32_t lim = p > 4096 ? p - 4096 : 0;
    while (p > lim && isSpace(tb_.at(p - 1))) --p;
    while (p > lim && !isSpace(tb_.at(p - 1))) --p;
    return p;
}

uint32_t App::wordRight(uint32_t p) {
    const uint32_t size = tb_.size();
    const uint32_t lim = std::min(size, p + 4096);
    while (p < lim && !isSpace(tb_.at(p))) ++p;
    while (p < lim && isSpace(tb_.at(p))) ++p;
    return p;
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

void App::editFailed() { pd::ui::message(ui_, "Cannot edit", {tb_.lastError()}); invalidate(); }

bool App::deleteSelection() {
    if (!hasSel()) return false;
    uint32_t a, b;
    selRange(&a, &b);
    if (!tb_.erase(a, b - a)) { editFailed(); return false; }
    cursor_ = a;
    anchor_ = kNone;
    return true;
}

void App::typeText(const char* s, uint32_t n) {
    const bool grouped = hasSel() || overwrite_;
    if (grouped) tb_.beginGroup();
    deleteSelection();
    anchor_ = kNone;
    if (overwrite_ && cursor_ < tb_.size() && tb_.at(cursor_) != '\n') {
        const uint32_t nx = lay_.nextChar(cursor_);
        tb_.erase(cursor_, nx - cursor_);
    }
    if (tb_.insert(cursor_, s, n)) cursor_ += n;
    else editFailed();
    if (grouped) tb_.endGroup();
    afterEdit();
}

void App::afterEdit() {
    goal_ = -1;
    normalizeTop();
    ensureVisible();
    if (tb_.nearlyFull() && !warnedFull_) {
        warnedFull_ = true;
        setMessage("Lots of edits: save soon to free memory", 5000);
    }
}

void App::handleKey(const KeyEvent& ev) {
    const uint8_t k = ev.code;
    const uint32_t size = tb_.size();
    const bool vert = (k == key::Up || k == key::Down || k == key::PageUp || k == key::PageDown);
    if (!vert || ev.ctrl) goal_ = -1;

    if (ev.ctrl) {
        switch (k) {
            case 'n': cmdNew(); return;
            case 'o': cmdOpen(); return;
            case 's': ev.shift ? cmdSaveAs() : cmdSave(); return;
            case 'f': cmdFind(); return;
            case 'g': cmdFindNext(); return;
            case 'r': cmdReplace(); return;
            case 'l': cmdGoto(); return;
            case 'z': cmdUndo(false); return;
            case 'y': cmdUndo(true); return;
            case 't': cmdStats(); return;
            case 'c': cmdCopy(false); return;
            case 'x': cmdCopy(true); return;
            case 'v': cmdPaste(); return;
            case 'a':
                anchor_ = 0;
                cursor_ = size;
                ensureVisible();
                return;
            case key::Left: moveTo(wordLeft(cursor_), ev.shift); return;
            case key::Right: moveTo(wordRight(cursor_), ev.shift); return;
            case key::Up:
            case key::Home:
            case key::PageUp: moveTo(0, ev.shift); return;
            case key::Down:
            case key::End:
            case key::PageDown: moveTo(size, ev.shift); return;
            default: return;
        }
    }

    uint8_t kk = k;
    if (ev.alt) {  // alternatives for keyboards without Home/End/PgUp/PgDn
        if (k == key::Left) kk = key::Home;
        else if (k == key::Right) kk = key::End;
        else if (k == key::Up) kk = key::PageUp;
        else if (k == key::Down) kk = key::PageDown;
    }

    switch (kk) {
        case key::Left:
            if (hasSel() && !ev.shift) { uint32_t a, b; selRange(&a, &b); moveTo(a, false); }
            else moveTo(lay_.prevChar(cursor_), ev.shift);
            return;
        case key::Right:
            if (hasSel() && !ev.shift) { uint32_t a, b; selRange(&a, &b); moveTo(b, false); }
            else moveTo(lay_.nextChar(cursor_), ev.shift);
            return;
        case key::Up: moveTo(vertical(cursor_, -1), ev.shift); return;
        case key::Down: moveTo(vertical(cursor_, +1), ev.shift); return;
        case key::Home: moveTo(lay_.rowContaining(cursor_).start, ev.shift); return;
        case key::End: moveTo(lay_.rowLastPos(lay_.rowContaining(cursor_)), ev.shift); return;
        case key::PageUp:
        case key::PageDown: {
            const int dir = kk == key::PageUp ? -1 : 1;
            uint32_t p = cursor_;
            for (int i = 0; i < rows_ - 1; ++i) p = vertical(p, dir);
            for (int i = 0; i < rows_ - 1; ++i) {
                if (dir < 0) {
                    if (top_ == 0) break;
                    top_ = lay_.prevRowStart(top_);
                } else {
                    const Row r = lay_.row(top_);
                    if (r.next == r.start || (r.next == size && !r.nl)) break;
                    top_ = r.next;
                }
            }
            moveTo(p, ev.shift);
            return;
        }
        case key::Esc:
            if (hasSel()) { anchor_ = kNone; return; }
            cmdMenu();
            return;
        case key::F1: showHelp(); relayout(); return;
        case key::F2:
            set_.font = (set_.font + 1) % pd::gfx::FONT_COUNT;
            relayout();
            saveSettings();
            return;
        case key::F3: cmdFindNext(); return;
        case key::F4:
            set_.status = !set_.status;
            relayout();
            saveSettings();
            return;
        case key::F5:
            set_.theme = (set_.theme + 1) % pd::ui::kThemeCount;
            relayout();
            setMessage(std::string("Theme: ") + th().name);
            saveSettings();
            return;
        case key::F6: cmdToggleSyntax(); return;
        case key::F7: cmdExportEpub(); return;
        case key::F8: cmdSendToX3(); return;
        case key::F9: cmdWifi(); return;
        case key::Insert: overwrite_ = !overwrite_; statusCache_.clear(); return;
        case key::Backspace:
            if (!deleteSelection() && cursor_ > 0) {
                const uint32_t p = lay_.prevChar(cursor_);
                if (tb_.erase(p, cursor_ - p)) cursor_ = p;
                else editFailed();
            }
            afterEdit();
            return;
        case key::Del:
            if (!deleteSelection() && cursor_ < size) {
                if (!tb_.erase(cursor_, lay_.nextChar(cursor_) - cursor_)) editFailed();
            }
            afterEdit();
            return;
        case key::Tab: typeText(kTab, 4); return;
        case key::Enter: {
            // Auto-indent: repeat the current line's leading whitespace.
            std::string ins = "\n";
            for (uint32_t p = lay_.lineStart(cursor_); p < cursor_ && ins.size() < 64; ++p) {
                const uint8_t c = tb_.at(p);
                if (c != ' ' && c != '\t') break;
                ins += char(c);
            }
            typeText(ins.data(), uint32_t(ins.size()));
            return;
        }
        default:
            if (const char c = ev.ch()) typeText(&c, 1);
            return;
    }
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

std::string App::docName() const {
    return tb_.path().empty() ? std::string("untitled") : pd::path::base_name(tb_.path());
}

void App::cmdMenu() {
    // Each entry carries its command, so the list can be reordered or extended
    // freely: nothing depends on an item's position.
    enum Cmd {
        kNew, kOpen, kSave, kSaveAs, kFind, kFindNext, kReplace, kGoto, kUndo, kRedo, kStats,
        kSetting, kExport, kSend, kWifi, kShare, kHelp, kDisplayTest, kRestart
    };
    static const char* const kFontNames[pd::gfx::FONT_COUNT] = {"6x12", "8x16", "10x20", "12x24"};
    static const char* const kOnOff[] = {"Off", "On"};
    static const char* themeNames[pd::ui::kThemeCount];
    for (int i = 0; i < pd::ui::kThemeCount; ++i) themeNames[i] = pd::ui::theme(i).name;

    // Settings are edited in place with Left/Right (or Enter) and applied when
    // the menu closes.
    int font = set_.font, theme = set_.theme, status = set_.status, syntax = set_.syntax;

    struct Entry {
        Cmd cmd;
        pd::ui::MenuItem item;
    };
    const Entry entries[] = {
        {kNew, pd::ui::action("New", "Ctrl+N")},
        {kOpen, pd::ui::action("Open...", "Ctrl+O")},
        {kSave, pd::ui::action("Save", "Ctrl+S")},
        {kSaveAs, pd::ui::action("Save as...", "C+Sh+S")},
        {kFind, pd::ui::action("Find...", "Ctrl+F")},
        {kFindNext, pd::ui::action("Find next", "F3")},
        {kReplace, pd::ui::action("Replace all...", "Ctrl+R")},
        {kGoto, pd::ui::action("Go to line...", "Ctrl+L")},
        {kUndo, pd::ui::action("Undo", "Ctrl+Z")},
        {kRedo, pd::ui::action("Redo", "Ctrl+Y")},
        {kStats, pd::ui::action("Statistics", "Ctrl+T")},
        {kSetting, pd::ui::choice("Font size", &font, kFontNames, pd::gfx::FONT_COUNT)},
        {kSetting, pd::ui::choice("Theme", &theme, themeNames, pd::ui::kThemeCount)},
        {kSetting, pd::ui::choice("Status bar", &status, kOnOff, 2)},
        {kSetting, pd::ui::choice("Syntax colours", &syntax, kOnOff, 2)},
        {kExport, pd::ui::action("Export ePub...", "F7")},
        {kSend, pd::ui::action("Send to X3...", "F8")},
        {kWifi, pd::ui::action("Wi-Fi...", "F9")},
        {kShare, pd::ui::action("ePub & X3 settings...")},
        {kHelp, pd::ui::action("Help", "F1")},
        {kDisplayTest, pd::ui::action("Display test")},
        {kRestart, pd::ui::action("Restart")},
    };
    std::vector<pd::ui::MenuItem> items;
    for (const Entry& e : entries) items.push_back(e.item);

    static int last = 0;
    pd::ui::MenuOptions opt;
    opt.initial = last;
    opt.footer = "Left/Right: change a setting";
    const int r = pd::ui::menu(ui_, "Menu", items, opt);
    if (r >= 0) last = r;

    // Apply the settings first: the chosen command then runs with them.
    const int themeBefore = set_.theme;
    const bool syntaxChanged = (syntax != 0) != set_.syntax;
    const bool changed = font != set_.font || theme != set_.theme || (status != 0) != set_.status ||
                         syntaxChanged;
    set_.font = font;
    set_.theme = theme;
    set_.status = status != 0;
    invalidate();
    relayout();
    if (syntaxChanged) applySyntax(syntax != 0);  // saves and reports
    else if (changed) saveSettings();
    if (theme != themeBefore) setMessage(std::string("Theme: ") + th().name);
    if (r < 0) return;
    switch (entries[r].cmd) {
        case kNew: cmdNew(); break;
        case kOpen: cmdOpen(); break;
        case kSave: cmdSave(); break;
        case kSaveAs: cmdSaveAs(); break;
        case kFind: cmdFind(); break;
        case kFindNext: cmdFindNext(); break;
        case kReplace: cmdReplace(); break;
        case kGoto: cmdGoto(); break;
        case kUndo: cmdUndo(false); break;
        case kRedo: cmdUndo(true); break;
        case kStats: cmdStats(); break;
        case kSetting: break;  // choice items never close the menu
        case kExport: cmdExportEpub(); break;
        case kSend: cmdSendToX3(); break;
        case kWifi: cmdWifi(); break;
        case kShare: cmdShareSettings(); break;
        case kHelp: showHelp(); relayout(); break;
        case kDisplayTest: cmdDisplayTest(); relayout(); break;
        case kRestart: cmdRestart(); break;
    }
}

void App::cmdToggleSyntax() { applySyntax(!set_.syntax); }

void App::applySyntax(bool on) {
    set_.syntax = on;
    invalidate();
    saveSettings();
    const syn::Lang l = syn::langForPath(tb_.path().c_str());
    if (!set_.syntax) setMessage("Syntax colours off");
    else if (l == syn::None) setMessage("Syntax colours on (.md .xml .py .cpp .h ...)");
    else setMessage(std::string("Syntax colours on: ") + syn::langName(l));
}

void App::cmdDisplayTest() {
    const Font& f = pd::gfx::font(1);
    const pd::Color white = pd::rgb(255, 255, 255), black = 0;
    struct Bar { const char* name; pd::Color c; pd::Color text; };
    const Bar bars[] = {
        {"red", pd::rgb(255, 0, 0), white},         {"green", pd::rgb(0, 255, 0), black},
        {"blue", pd::rgb(0, 0, 255), white},        {"white", white, black},
        {"mid grey", pd::rgb(128, 128, 128), black}, {"dark grey", pd::rgb(34, 34, 40), white},
        {"orange", pd::rgb(232, 160, 64), black},   {"black", black, white},
    };
    pd::gfx::fill(0, 0, pd::gfx::width(), pd::gfx::height(), black);
    const int bh = 30;
    for (int i = 0; i < 8; ++i) {
        pd::gfx::fill(0, 8 + i * bh, pd::gfx::width(), bh, bars[i].c);
        pd::gfx::text(8, 8 + i * bh + 7, 200, f, bars[i].name, bars[i].text, bars[i].c);
    }
    pd::gfx::frame(0, 0, pd::gfx::width(), pd::gfx::height(), white);
    char line[64];
    snprintf(line, sizeof line, "Test 1/2  theme %s  (key)", th().name);
    pd::gfx::text(8, pd::gfx::height() - 60, pd::gfx::width() - 16, f, line, white, black);
    kbd_.wait();

    const pd::Color fills[] = {pd::rgb(255, 0, 0), pd::rgb(0, 255, 0), pd::rgb(0, 0, 255),
                              white};
    for (pd::Color c : fills) {
        pd::gfx::fill(0, 0, pd::gfx::width(), pd::gfx::height(), c);
        pd::sys::sleep_ms(500);
    }
    pd::gfx::fill(0, 0, pd::gfx::width(), pd::gfx::height(), black);
    pd::gfx::text(8, 100, pd::gfx::width() - 16, f, "Test 2/2 done:", white, black);
    pd::gfx::text(8, 120, pd::gfx::width() - 16, f, "red, green, blue, white", white, black);
    pd::gfx::text(8, 140, pd::gfx::width() - 16, f, "should have filled the", white, black);
    pd::gfx::text(8, 160, pd::gfx::width() - 16, f, "whole screen. (key)", white, black);
    kbd_.wait();
    invalidate();
}

bool App::maybeSaveChanges() {
    if (!tb_.modified()) return true;
    const int r = pd::ui::ask(ui_, "Unsaved changes", "Save changes to " + docName() + "?");
    relayout();
    if (r < 0) return false;
    if (r == 1) return cmdSave();
    return true;
}

void App::cmdNew() {
    if (!maybeSaveChanges()) return;
    if (!tb_.newDocument()) editFailed();
    cursor_ = top_ = 0;
    anchor_ = kNone;
    warnedFull_ = false;
    relayout();
    saveSettings();
}

void App::cmdOpen() {
    if (!maybeSaveChanges()) return;
    std::string path;
    pd::ui::BrowseOptions bo;
    bo.hide = {".fwedit"};
    const bool ok = pd::ui::browse(ui_, "Open", set_.cwd, path, bo);
    relayout();
    if (!ok) return;
    if (!tb_.load(path)) {
        pd::ui::message(ui_, "Open failed", {tb_.lastError()});
        relayout();
        return;
    }
    cursor_ = top_ = 0;
    anchor_ = kNone;
    warnedFull_ = false;
    relayout();
    saveSettings();
    setMessage("Opened " + pd::path::base_name(path));
}

bool App::saveTo(const std::string& path) {
    const bool ok = tb_.saveTo(path, progressFn("Saving", true));
    msg_.clear();
    if (!ok) {
        if (tb_.reopenFailed()) {
            cursor_ = 0;
            anchor_ = kNone;
            top_ = 0;
            pd::ui::message(ui_, "Saved", {"The file was saved, but it could not be",
                                        "reopened. Open it again with Ctrl+O.", tb_.lastError()});
            relayout();
            return true;
        }
        pd::ui::message(ui_, "Save failed", {tb_.lastError()});
        relayout();
        return false;
    }
    cursor_ = std::min(cursor_, tb_.size());
    anchor_ = kNone;
    warnedFull_ = false;
    set_.cwd = pd::path::parent(path);
    saveSettings();
    relayout();
    setMessage("Saved " + pd::path::base_name(path));
    return true;
}

bool App::cmdSave() {
    if (tb_.path().empty()) return cmdSaveAs();
    return saveTo(tb_.path());
}

bool App::cmdSaveAs() {
    std::string v = tb_.path().empty() ? pd::path::join(set_.cwd, "untitled.txt") : tb_.path();
    const bool ok = pd::ui::prompt(ui_, "Save as (full path)", v, "Enter = save   Esc = cancel");
    relayout();
    if (!ok || v.empty()) return false;
    if (v[0] != '/') v = pd::path::join(set_.cwd, v);
    if (v.back() == '/') {
        pd::ui::message(ui_, "Save as", {"Please type a file name."});
        relayout();
        return false;
    }
    if (!fs::is_dir(pd::path::parent(v).c_str())) {
        pd::ui::message(ui_, "Save as", {"Folder " + pd::path::parent(v) + " does not exist."});
        relayout();
        return false;
    }
    if (strcasecmp(v.c_str(), tb_.path().c_str()) != 0 && fs::exists(v.c_str())) {
        const int r = pd::ui::ask(ui_, "File exists", "Overwrite " + pd::path::base_name(v) + "?", false);
        relayout();
        if (r != 1) return false;
    }
    return saveTo(v);
}

void App::cmdFind() {
    std::string v = lastFind_;
    const bool ok = pd::ui::prompt(ui_, "Find (ignores case)", v);
    relayout();
    if (!ok || v.empty()) return;
    lastFind_ = v;
    cmdFindNext();
}

void App::cmdFindNext() {
    if (lastFind_.empty()) { cmdFind(); return; }
    const uint32_t p = tb_.find(lastFind_, cursor_, true, false, progressFn("Searching", true));
    msg_.clear();
    if (p == UINT32_MAX) {
        setMessage("Not found: " + lastFind_);
        return;
    }
    anchor_ = p;
    cursor_ = p + uint32_t(lastFind_.size());
    tb_.breakUndoMerge();
    if (p < top_) centerCursor();
    ensureVisible();
}

void App::cmdReplace() {
    std::string f = lastFind_;
    if (!pd::ui::prompt(ui_, "Replace all: find", f) || f.empty()) { relayout(); return; }
    std::string r = lastReplace_;
    if (!pd::ui::prompt(ui_, "Replace with", r)) { relayout(); return; }
    relayout();
    lastFind_ = f;
    lastReplace_ = r;
    uint32_t count = 0, pos = 0, lastHit = cursor_;
    bool failed = false;
    tb_.beginGroup();
    for (;;) {
        const uint32_t p = tb_.find(f, pos, false, false, progressFn("Replacing", true));
        if (p == UINT32_MAX) break;
        if (!tb_.erase(p, uint32_t(f.size())) || !tb_.insert(p, r.data(), uint32_t(r.size()))) {
            failed = true;
            break;
        }
        ++count;
        pos = p + uint32_t(r.size());
        lastHit = pos;
    }
    tb_.endGroup();
    msg_.clear();
    anchor_ = kNone;
    cursor_ = std::min(lastHit, tb_.size());
    afterEdit();
    if (failed) editFailed();
    char b[64];
    snprintf(b, sizeof b, "Replaced %lu (Ctrl+Z undoes all)", (unsigned long)count);
    setMessage(b, 4000);
}

void App::cmdGoto() {
    std::string v;
    const bool ok = pd::ui::prompt(ui_, "Go to line", v);
    relayout();
    if (!ok) return;
    const long target = atol(v.c_str());
    if (target < 1) return;
    uint32_t line = 1, pos = 0;
    const uint32_t size = tb_.size();
    auto prog = progressFn("Seeking", true);
    while (line < uint32_t(target) && pos < size) {
        const uint32_t n = tb_.read(pos, g_chunk, sizeof g_chunk);
        if (n == 0) break;
        uint32_t i = 0;
        for (; i < n; ++i) {
            if (g_chunk[i] == '\n' && ++line == uint32_t(target)) { ++i; break; }
        }
        pos += i;
        if (!prog(pos, size)) break;
    }
    msg_.clear();
    moveTo(pos, false);
    centerCursor();
    if (line < uint32_t(target)) setMessage("File has only " + std::to_string(line) + " lines");
}

void App::cmdStats() {
    const uint32_t size = tb_.size();
    uint32_t chars = 0, words = 0, lines = 1, curLine = 1;
    bool inWord = false;
    auto prog = progressFn("Counting", true);
    for (uint32_t pos = 0; pos < size;) {
        const uint32_t n = tb_.read(pos, g_chunk, sizeof g_chunk);
        if (n == 0) break;
        for (uint32_t i = 0; i < n; ++i) {
            const uint8_t c = g_chunk[i];
            if (!Layout::isCont(c)) ++chars;
            if (c == '\n') {
                ++lines;
                if (pos + i < cursor_) ++curLine;
            }
            const bool sp = isSpace(c);
            if (!sp && !inWord) ++words;
            inWord = !sp;
        }
        pos += n;
        if (!prog(pos, size)) { msg_.clear(); return; }
    }
    msg_.clear();
    char b[5][48];
    snprintf(b[0], 48, "Size:       %lu bytes", (unsigned long)size);
    snprintf(b[1], 48, "Characters: %lu", (unsigned long)chars);
    snprintf(b[2], 48, "Words:      %lu", (unsigned long)words);
    snprintf(b[3], 48, "Lines:      %lu (cursor on %lu)", (unsigned long)lines, (unsigned long)curLine);
    snprintf(b[4], 48, "Edit memory: %u%% used",
             unsigned(tb_.pieceCount() * 100 / TextBuffer::kMaxPieces));
    pd::ui::message(ui_, docName().c_str(), {b[0], b[1], b[2], b[3], b[4]});
    relayout();
}

void App::cmdUndo(bool redo) {
    uint32_t c = cursor_;
    const bool ok = redo ? tb_.redo(&c) : tb_.undo(&c);
    if (!ok) {
        if (!tb_.lastError().empty() && (redo ? tb_.canRedo() : tb_.canUndo())) editFailed();
        else setMessage(redo ? "Nothing to redo" : "Nothing to undo");
        return;
    }
    cursor_ = std::min(c, tb_.size());
    anchor_ = kNone;
    afterEdit();
}

void App::cmdCopy(bool cut) {
    if (!hasSel()) {
        setMessage("Select text first (Shift+arrows)");
        return;
    }
    uint32_t a, b;
    selRange(&a, &b);
    if (!tb_.copyToFile(a, b - a, kClipPath)) {
        editFailed();
        return;
    }
    if (cut) {
        deleteSelection();
        afterEdit();
    }
    char m[48];
    snprintf(m, sizeof m, "%s %lu bytes", cut ? "Cut" : "Copied", (unsigned long)(b - a));
    setMessage(m);
}

void App::cmdPaste() {
    tb_.beginGroup();
    deleteSelection();
    uint32_t len = 0;
    const bool ok = tb_.insertFile(cursor_, kClipPath, &len);
    tb_.endGroup();
    if (!ok) { setMessage(tb_.lastError()); return; }
    cursor_ += len;
    anchor_ = kNone;
    afterEdit();
}

void App::cmdRestart() {
    if (!maybeSaveChanges()) return;
    saveSettings();
    pd::sys::reboot();
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void App::run() {
    relayout();  // uses default theme until settings are read
    diag::stage(diag::kSdCard);
    waitForCard();
    diag::stage(diag::kWorkDir);
    fs::mkdir(kWorkDir);
    diag::stage(diag::kScratch);
    if (!tb_.init(kWorkDir)) {
        pd::ui::message(ui_, "SD card error", {tb_.lastError()});
    }
    diag::stage(diag::kSettings);
    loadSettings();
    wifi_.load();
    // Safe mode: if the previous start died while reopening the last file,
    // start with an empty document instead of trying again.
    const bool skipLast = diag::previousStage() == diag::kLastFile;
    diag::stage(diag::kLastFile);
    if (!skipLast && !set_.file.empty() && fs::exists(set_.file.c_str()) &&
        tb_.load(set_.file)) {
        cursor_ = std::min(set_.pos, tb_.size());
    }
    diag::stage(diag::kFirstScreen);
    relayout();
    centerCursor();
    setMessage(skipLast ? "Safe start: last file not reopened" : "F1 help  -  Esc menu", 4000);

    bool dirty = true;
    bool started = false;
    for (;;) {
        KeyEvent ev;
        if (kbd_.poll(ev)) {
            handleKey(ev);
            dirty = true;
            continue;
        }
        const uint32_t t = nowMs();
        if (!msg_.empty() && int32_t(t - msgUntil_) >= 0) {
            msg_.clear();
            statusCache_.clear();
            dirty = true;
        }
        if (int32_t(t - nextBattery_) >= 0) {
            nextBattery_ = t + 30000;
            const int b = batteryRaw();
            if (b != battery_) { battery_ = b; dirty = true; }
        }
        if (net::is_on()) {
            net::poll();
            checkWifiIdle();
        }
        if (dirty) {
            render();
            dirty = false;
            if (!started) { started = true; diag::done(); }
        }
        pd::sys::sleep_ms(2);
    }
}

// ---------------------------------------------------------------------------
// Help screen (PicoPods' scrollable text view)
// ---------------------------------------------------------------------------

void App::showHelp() {
    pd::ui::text_view(ui_, "Pod Writer " FW_VERSION " - keyboard",
                      {
                          "Esc              Menu",
                          "Ctrl+N  Ctrl+O   New / Open",
                          "Ctrl+S           Save",
                          "Ctrl+Shift+S     Save as",
                          "Ctrl+F  Ctrl+G   Find / Find next (also F3)",
                          "Ctrl+R           Replace all",
                          "Ctrl+L           Go to line",
                          "Ctrl+Z  Ctrl+Y   Undo / Redo",
                          "Ctrl+A           Select all",
                          "Ctrl+C/X/V       Copy / Cut / Paste",
                          "Shift+arrows     Select text",
                          "Ctrl+Left/Right  Previous / next word",
                          "Home  End        Row start / end (Alt+L/R)",
                          "PgUp  PgDn       Page up / down  (Alt+U/D)",
                          "Ctrl+Up/Down     Start / end of document",
                          "Ins              Insert / overwrite",
                          "Tab              Indent (4 spaces)",
                          "F1 help  F2 font size  F4 status bar",
                          "F5 theme  F6 syntax colours  Ctrl+T stats",
                          "F7 export ePub  F8 send to X3  F9 Wi-Fi",
                          "",
                          "Files live on the SD card; only your edits",
                          "use RAM, so huge files are fine.",
                      });
}
