// Pod Files - a file manager for the SD card, built on the PicoPods libraries.
//
// Browse, mark, copy / cut / paste, delete, rename and create folders, and a
// "USB drive" mode that shows the SD card to a computer through the Pico's
// micro-USB port.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <strings.h>
#include <vector>

#include "pd/bt.h"
#include "pd/diag.h"
#include "pd/fs_ops.h"
#include "pd/git.h"
#include "pd/tls.h"
#include "pd/wifi.h"
#include "pd/pd.h"
#include "pd/usb_drive.h"

namespace {

using pd::Color;
using pd::fs::Conflict;
namespace key = pd::key;
namespace ui = pd::ui;
namespace gfx = pd::gfx;

const char* const kStages[] = {"start", "display", "SD card", "settings", "ready"};
enum Stage { kStart, kDisplay, kSd, kSettings, kReady };

const char* const kWorkDir = "/.podfiles";
const char* const kOldWorkDir = "/.picocalc_files";   // name used up to PicoCalc Files 1.2
const char* const kSettingsPath = "/.podfiles/settings.txt";
const char* const kWifiPath = "/.podfiles/wifi.txt";
const char* const kCaPath = "/.podfiles/ca.pem";   // extra trusted CAs (self-hosted servers)
const char* const kBtKeys = "/.podfiles/bt_keys.bin";
constexpr size_t kMaxEntries = 2000;

const char* const kSortNames[] = {"Name", "Size", "Type"};
const char* const kFontNames[] = {"Small", "Normal"};
const char* const kThemeNames[] = {"Night", "Paper", "Phosphor"};
const char* const kOnOff[] = {"Off", "On"};
const char* const kHistory[] = {"Latest commit", "Full history"};

// ------------------------------------------------------------------ helpers
std::string size_text(uint64_t n) {
    char b[24];
    if (n < 1024) snprintf(b, sizeof b, "%uB", unsigned(n));
    else if (n < 10ull * 1024) snprintf(b, sizeof b, "%.1fK", double(n) / 1024.0);
    else if (n < 1024ull * 1024) snprintf(b, sizeof b, "%uK", unsigned(n / 1024));
    else if (n < 10ull * 1024 * 1024) snprintf(b, sizeof b, "%.1fM", double(n) / 1048576.0);
    else if (n < 1024ull * 1024 * 1024) snprintf(b, sizeof b, "%uM", unsigned(n / 1048576));
    else snprintf(b, sizeof b, "%.1fG", double(n) / 1073741824.0);
    return b;
}

std::string plural(uint32_t n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

// Keeps the end of a long path: "...ooks/2026/notes".
std::string clip_left(const std::string& s, int cells) {
    const int n = gfx::text_cells(s);
    if (n <= cells) return s;
    if (cells <= 3) return gfx::clip_cells(s, cells);
    return "..." + s.substr(gfx::byte_of_cell(s, n - (cells - 3)));
}

std::string lower_ext(const std::string& name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return "";
    std::string e = name.substr(dot + 1);
    for (char& c : e) c = char(tolower(uint8_t(c)));
    return e;
}

// ------------------------------------------------------------------ state
struct Entry {
    std::string name;
    bool is_dir = false;
    uint32_t size = 0;
    bool marked = false;
};

struct Settings {
    int theme = 0;
    int show_hidden = 0;
    int sort = 0;
    int font = 1;   // 0 = 6x12, 1 = 8x16
    std::string dir = "/";
    std::string git_url;
    int git_full = 0;
};

struct Clipboard {
    std::vector<std::string> paths;
    bool cut = false;
    bool empty() const { return paths.empty(); }
};

class FileManager {
public:
    FileManager(pd::Terminal& t, ui::Context& u) : term_(t), ui_(u), wifi_(kWifiPath) {}

    void run();

private:
    // --- setup and persistence
    void load_settings();
    void save_settings();

    // --- listing
    void reload(const std::string& select = std::string());
    void sort_entries();
    void refresh_space();
    std::string current_path() const;
    std::vector<std::string> selection() const;
    int marked_count() const;
    bool in_clipboard_cut(const std::string& name) const;

    // --- drawing
    const gfx::Font& list_font() const { return gfx::font(s_.font ? gfx::FONT_8x16 : gfx::FONT_6x12); }
    int list_top() const { return bar_.height() + gfx::font(gfx::FONT_8x16).h + 2; }
    int footer_h() const { return gfx::font(gfx::FONT_6x12).h + 3; }
    int rows() const { return (gfx::height() - list_top() - footer_h()) / list_font().h; }
    void draw_all();
    void draw_header();
    void draw_row(int i);
    void draw_rows();
    void draw_footer();
    void draw_status();
    void toast(const std::string& msg);

    // --- actions
    void open_selected();
    void go_up();
    void toggle_mark();
    void mark_all();
    void to_clipboard(bool cut);
    void paste();
    void remove_selected();
    void new_folder();
    void rename_selected();
    void show_info();
    void usb_mode();
    void git_clone();
    void bt_menu();
    bool bt_on();
    void bt_off();
    bool bt_pairing();                     // answers a pending pairing request (dialog)
    void bt_idle();                        // ui.idle while Bluetooth is on: poll + pairing from any screen
    std::string bt_pick_device();          // "" = cancelled
    void bt_send(const std::vector<std::string>& files);
    void bt_receive();
    void file_actions();
    void main_menu();
    void help();
    void about();
    void apply_theme();

    // --- operation UI
    bool ask_stop();
    Conflict ask_conflict(const std::string& target, bool src_dir, bool dst_dir);
    void report(const char* verb, const pd::fs::OpResult& r);

    pd::Terminal& term_;
    ui::Context& ui_;
    pd::WifiManager wifi_;
    bool ca_loaded_ = false;
    std::vector<std::string> bt_log_tail_;
    bool in_bt_dialog_ = false;
    std::string bt_trail_;
    std::vector<std::string> bt_stack_log_;
    void draw_bt_live(const char* title, const std::vector<std::string>& lines);   // Bluetooth event log at the end of the last send
    Settings s_;
    pd::app::StatusBar bar_;
    std::string dir_ = "/";
    std::vector<Entry> entries_;
    bool truncated_ = false;
    int sel_ = 0, top_ = 0;
    int drawn_sel_ = -1, drawn_top_ = -1;
    Clipboard clip_;
    uint64_t free_ = 0, total_ = 0;
    bool space_known_ = false;
    std::string toast_;
    uint32_t toast_until_ = 0;
    bool dirty_all_ = true;
    bool dirty_footer_ = true;
    int sticky_[3] = {-1, -1, -1};
    uint32_t dir_save_at_ = 0;   // conflict answer "for all": file, folder, mixed

    // progress panel state
    std::string op_title_;
    int op_last_pmil_ = -1;
    bool op_force_ = true;
    uint32_t op_next_esc_ = 0;
};

// ================================================================== settings
void FileManager::load_settings() {
    pd::Settings st;
    st.load(kSettingsPath);
    s_.theme = st.get_int("theme", 0, 0, ui::kThemeCount - 1);
    s_.show_hidden = st.get_int("show_hidden", 0, 0, 1);
    s_.sort = st.get_int("sort", 0, 0, 2);
    s_.font = st.get_int("font", 1, 0, 1);
    s_.dir = st.get("dir", "/");
    s_.git_url = st.get("git_url", "");
    s_.git_full = st.get_int("git_full", 0, 0, 1);
    if (s_.dir.empty() || s_.dir[0] != '/') s_.dir = "/";
}

void FileManager::save_settings() {
    if (!pd::fs::mounted()) return;
    pd::fs::mkdirs(kWorkDir);
    pd::Settings st;
    st.set_int("theme", s_.theme);
    st.set_int("show_hidden", s_.show_hidden);
    st.set_int("sort", s_.sort);
    st.set_int("font", s_.font);
    st.set("dir", dir_);
    st.set("git_url", s_.git_url);
    st.set_int("git_full", s_.git_full);
    st.save(kSettingsPath);
}

void FileManager::apply_theme() { ui_.th = &ui::theme(s_.theme); }

// ================================================================== listing
void FileManager::reload(const std::string& select) {
    std::string want = select;
    if (want.empty() && sel_ >= 0 && sel_ < int(entries_.size())) want = entries_[size_t(sel_)].name;

    while (dir_ != "/" && !pd::fs::is_dir(dir_.c_str())) {
        want = pd::path::base_name(dir_);
        dir_ = pd::path::parent(dir_);
    }
    // Keep marks on entries that are still there.
    std::vector<std::string> marked;
    for (const Entry& e : entries_)
        if (e.marked) marked.push_back(e.name);

    std::vector<pd::fs::DirEntry> list;
    pd::fs::list_dir(dir_.c_str(), list, kMaxEntries + 1, s_.show_hidden != 0);
    truncated_ = list.size() > kMaxEntries;
    if (truncated_) list.resize(kMaxEntries);

    entries_.clear();
    entries_.reserve(list.size() + 1);
    if (dir_ != "/") entries_.push_back(Entry{"..", true, 0, false});
    for (const pd::fs::DirEntry& d : list) {
        Entry e{d.name, d.is_dir, d.size, false};
        e.marked = std::find(marked.begin(), marked.end(), d.name) != marked.end();
        entries_.push_back(std::move(e));
    }
    sort_entries();

    sel_ = 0;
    for (size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].name == want) {
            sel_ = int(i);
            break;
        }
    top_ = std::max(0, sel_ - rows() / 2);
    dirty_all_ = true;
}

void FileManager::sort_entries() {
    const size_t first = (!entries_.empty() && entries_[0].name == "..") ? 1 : 0;
    const int mode = s_.sort;
    std::stable_sort(entries_.begin() + long(first), entries_.end(), [mode](const Entry& a, const Entry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir;
        if (mode == 1 && !a.is_dir && a.size != b.size) return a.size > b.size;
        if (mode == 2 && !a.is_dir) {
            const std::string ea = lower_ext(a.name), eb = lower_ext(b.name);
            if (ea != eb) return ea < eb;
        }
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
}

void FileManager::refresh_space() { space_known_ = pd::fs::space(&total_, &free_); }

std::string FileManager::current_path() const {
    if (sel_ < 0 || sel_ >= int(entries_.size())) return "";
    const Entry& e = entries_[size_t(sel_)];
    if (e.name == "..") return "";
    return pd::path::join(dir_, e.name);
}

std::vector<std::string> FileManager::selection() const {
    std::vector<std::string> out;
    for (const Entry& e : entries_)
        if (e.marked) out.push_back(pd::path::join(dir_, e.name));
    if (out.empty()) {
        const std::string p = current_path();
        if (!p.empty()) out.push_back(p);
    }
    return out;
}

int FileManager::marked_count() const {
    int n = 0;
    for (const Entry& e : entries_) n += e.marked ? 1 : 0;
    return n;
}

bool FileManager::in_clipboard_cut(const std::string& name) const {
    if (!clip_.cut || clip_.empty()) return false;
    const std::string p = pd::path::join(dir_, name);
    for (const std::string& c : clip_.paths)
        if (pd::fs::same_path(c, p)) return true;
    return false;
}

// ================================================================== drawing
void FileManager::draw_status() {
    std::string left = "Files";
    if (space_known_) left += "  " + size_text(free_) + " free";
    const int m = marked_count();
    const std::string right = m ? std::to_string(m) + " marked" : "";
    bar_.draw(ui_.theme(), left, right);
}

void FileManager::draw_header() {
    const ui::Theme& t = ui_.theme();
    const gfx::Font& f = gfx::font(gfx::FONT_8x16);
    const int y = bar_.height();
    const int cells = (gfx::width() - 8) / f.w;
    gfx::fill(0, y, gfx::width(), f.h + 2, t.bg);
    gfx::text(4, y + 1, gfx::width() - 8, f, gfx::pad_cells(clip_left(dir_, cells), cells), t.accent, t.bg);
    gfx::hline(0, y + f.h + 1, gfx::width(), t.dim);
}

void FileManager::draw_row(int i) {
    const ui::Theme& t = ui_.theme();
    const gfx::Font& f = list_font();
    const int idx = top_ + i;
    const int y = list_top() + i * f.h;
    const int cells = (gfx::width() - 8) / f.w;
    std::string text;
    Color fg = t.fg, bg = t.bg;
    if (idx < int(entries_.size())) {
        const Entry& e = entries_[size_t(idx)];
        std::string right;
        if (e.name != "..") right = e.is_dir ? "<dir>" : size_text(e.size);
        const std::string label = std::string(e.marked ? "*" : " ") + e.name + (e.is_dir && e.name != ".." ? "/" : "");
        const int rc = right.empty() ? 0 : gfx::text_cells(right) + 1;
        text = gfx::pad_cells(label, cells - rc) + (right.empty() ? "" : " " + right);
        if (e.marked) fg = t.accent;
        else if (e.is_dir) fg = t.fg;
        else fg = t.fg;
        if (in_clipboard_cut(e.name)) fg = t.dim;
        if (idx == sel_) {
            bg = t.hi_bg;
            fg = e.marked ? t.accent : t.hi_fg;
            if (e.marked && t.accent == t.hi_bg) fg = t.hi_fg;
        }
    } else if (idx == 0) {
        text = "  (empty folder)";
        fg = t.dim;
    }
    gfx::text(4, y, gfx::width() - 8, f, gfx::pad_cells(text, cells), fg, bg);
}

void FileManager::draw_rows() {
    const int n = rows();
    if (sel_ < top_) top_ = sel_;
    if (sel_ >= top_ + n) top_ = sel_ - n + 1;
    if (top_ < 0) top_ = 0;
    if (dirty_all_ || top_ != drawn_top_) {
        for (int i = 0; i < n; ++i) draw_row(i);
        // clear the gap under the last row
        const int used = list_top() + n * list_font().h;
        gfx::fill(0, used, gfx::width(), gfx::height() - footer_h() - used, ui_.theme().bg);
    } else if (sel_ != drawn_sel_) {
        if (drawn_sel_ >= top_ && drawn_sel_ < top_ + n) draw_row(drawn_sel_ - top_);
        draw_row(sel_ - top_);
    }
    drawn_sel_ = sel_;
    drawn_top_ = top_;
}

void FileManager::draw_footer() {
    const ui::Theme& t = ui_.theme();
    const gfx::Font& f = gfx::font(gfx::FONT_6x12);
    const int y = gfx::height() - footer_h();
    const int cells = (gfx::width() - 8) / f.w;
    std::string msg;
    Color fg = t.status_fg;
    if (!toast_.empty() && !pd::sys::reached(toast_until_)) {
        msg = toast_;
        fg = t.accent;
    } else if (!clip_.empty()) {
        msg = plural(uint32_t(clip_.paths.size()), "item", "items") + (clip_.cut ? " cut" : " copied") +
              "   v: paste here   Esc: menu";
    } else if (truncated_) {
        msg = "Only the first " + std::to_string(kMaxEntries) + " entries are listed";
        fg = t.error;
    } else {
        msg = "Spc mark  c copy  x cut  d del  F1 help";
    }
    gfx::fill(0, y, gfx::width(), footer_h(), t.status_bg);
    gfx::text(4, y + 2, gfx::width() - 8, f, gfx::pad_cells(msg, cells), fg, t.status_bg);
}

void FileManager::draw_all() {
    gfx::clear(ui_.theme().bg);
    bar_.draw(ui_.theme(), "", "", true);
    draw_status();
    draw_header();
    dirty_all_ = true;
    draw_rows();
    draw_footer();
    dirty_all_ = false;
    dirty_footer_ = false;
}

void FileManager::toast(const std::string& msg) {
    toast_ = msg;
    toast_until_ = pd::sys::now_ms() + 4000;
    dirty_footer_ = true;
}

// ================================================================== navigation
void FileManager::open_selected() {
    if (sel_ >= int(entries_.size())) return;
    const Entry& e = entries_[size_t(sel_)];
    if (e.name == "..") {
        go_up();
        return;
    }
    if (!e.is_dir) {
        file_actions();
        return;
    }
    dir_ = pd::path::join(dir_, e.name);
    entries_.clear();
    reload();
    draw_all();
}

void FileManager::go_up() {
    if (dir_ == "/") return;
    const std::string from = pd::path::base_name(dir_);
    dir_ = pd::path::parent(dir_);
    entries_.clear();
    reload(from);
    draw_all();
}

void FileManager::toggle_mark() {
    if (sel_ >= int(entries_.size())) return;
    Entry& e = entries_[size_t(sel_)];
    if (e.name != "..") e.marked = !e.marked;
    draw_row(sel_ - top_);
    if (sel_ + 1 < int(entries_.size())) ++sel_;
    draw_rows();
    draw_status();
}

void FileManager::mark_all() {
    bool all = true;
    for (const Entry& e : entries_)
        if (e.name != ".." && !e.marked) all = false;
    for (Entry& e : entries_)
        if (e.name != "..") e.marked = !all;
    dirty_all_ = true;
    draw_rows();
    dirty_all_ = false;
    draw_status();
}

// ================================================================== operations UI
bool FileManager::ask_stop() {
    const bool stop = ui::confirm(ui_, op_title_.c_str(), "Stop now? What is already done stays done.");
    ui_.kbd.flush();
    op_force_ = true;
    return stop;
}

Conflict FileManager::ask_conflict(const std::string& target, bool src_dir, bool dst_dir) {
    const int kind = (src_dir && dst_dir) ? 1 : (!src_dir && !dst_dir) ? 0 : 2;
    if (sticky_[kind] >= 0) return Conflict(sticky_[kind]);

    const std::string name = gfx::clip_cells(pd::path::base_name(target), 34);
    const char* what = kind == 1   ? "This folder already exists here."
                       : kind == 0 ? "This file already exists here."
                       : dst_dir   ? "A folder here has this file's name."
                                   : "A file here has this folder's name.";
    struct Option {
        const char* label;
        Conflict c;
        bool all;
    };
    static const Option kFolder[] = {{"Merge the folders", Conflict::Merge, false},
                                     {"Keep both (rename)", Conflict::KeepBoth, false},
                                     {"Skip", Conflict::Skip, false},
                                     {"Merge all", Conflict::Merge, true},
                                     {"Skip all", Conflict::Skip, true},
                                     {"Cancel", Conflict::Cancel, false}};
    static const Option kFile[] = {{"Replace", Conflict::Replace, false},
                                   {"Keep both (rename)", Conflict::KeepBoth, false},
                                   {"Skip", Conflict::Skip, false},
                                   {"Replace all", Conflict::Replace, true},
                                   {"Skip all", Conflict::Skip, true},
                                   {"Cancel", Conflict::Cancel, false}};
    static const Option kMixed[] = {{"Keep both (rename)", Conflict::KeepBoth, false},
                                    {"Skip", Conflict::Skip, false},
                                    {"Replace (deletes it)", Conflict::Replace, false},
                                    {"Keep both for all", Conflict::KeepBoth, true},
                                    {"Skip all", Conflict::Skip, true},
                                    {"Cancel", Conflict::Cancel, false}};
    const Option* opts = kind == 1 ? kFolder : kind == 0 ? kFile : kMixed;
    constexpr int kOptions = 6;
    std::vector<ui::MenuItem> items;
    items.push_back(ui::info(name.c_str()));
    items.push_back(ui::info(what));
    for (int i = 0; i < kOptions; ++i) items.push_back(ui::action(opts[i].label));
    ui::MenuOptions mo;
    mo.initial = 2;
    const int r = ui::menu(ui_, "Already exists", items, mo);
    ui_.kbd.flush();
    op_force_ = true;
    if (r < 2) return Conflict::Cancel;
    const Option& o = opts[r - 2];
    if (o.all) sticky_[kind] = int(o.c);
    return o.c;
}

void FileManager::report(const char* verb, const pd::fs::OpResult& r) {
    std::string msg = std::string(verb) + " " + plural(r.done, "item", "items");
    if (r.skipped) msg += ", skipped " + std::to_string(r.skipped);
    if (r.cancelled) msg += " (stopped)";
    if (r.failed) {
        ui::message(ui_, "Some items failed",
                    {msg, plural(r.failed, "item", "items") + " could not be processed.", "First error:", r.error},
                    ui_.theme().error);
        dirty_all_ = true;
    }
    toast(msg);
}

// ================================================================== actions
void FileManager::to_clipboard(bool cut) {
    std::vector<std::string> sel = selection();
    if (sel.empty()) return;
    clip_.paths = sel;
    clip_.cut = cut;
    for (Entry& e : entries_) e.marked = false;
    toast(plural(uint32_t(sel.size()), "item", "items") + (cut ? " cut" : " copied") + " - v pastes");
    dirty_all_ = true;
}

void FileManager::paste() {
    if (clip_.empty()) {
        toast("Clipboard is empty: copy (c) or cut (x) first");
        return;
    }
    // The card may have changed (USB drive mode): drop what is gone.
    std::vector<std::string> src;
    for (const std::string& p : clip_.paths)
        if (pd::fs::exists(p.c_str())) src.push_back(p);
    if (src.empty()) {
        clip_.paths.clear();
        toast("The copied items no longer exist");
        return;
    }
    const bool cut = clip_.cut;
    op_title_ = cut ? "Moving" : "Copying";
    op_last_pmil_ = -1;
    op_force_ = true;
    op_next_esc_ = 0;
    sticky_[0] = sticky_[1] = sticky_[2] = -1;

    pd::fs::OpHooks h;
    h.progress = [this](uint64_t done, uint64_t total, const std::string& cur) {
        const int pmil = total ? int(std::min<uint64_t>(1000, done * 1000 / total)) : 0;
        if (op_force_ || pmil != op_last_pmil_) {
            const std::string name = pd::path::base_name(cur);
            char pct[16];
            snprintf(pct, sizeof pct, "%3d%%", pmil / 10);
            const int cells = (gfx::width() - 32) / ui_.font().w;
            const std::string label = gfx::pad_cells(gfx::clip_cells(name, cells - 5), cells - 4) + pct;
            if (op_force_) ui::progress(ui_, op_title_.c_str(), label, 0, 1000);
            ui::progress(ui_, op_title_.c_str(), label, uint32_t(std::max(1, pmil)), 1000);
            op_force_ = false;
            op_last_pmil_ = pmil;
        }
        if (pd::sys::reached(op_next_esc_)) {
            op_next_esc_ = pd::sys::now_ms() + 200;
            if (ui_.kbd.esc_pressed() && ask_stop()) return false;
        }
        return true;
    };
    h.conflict = [this](const std::string& t, bool sd, bool dd) { return ask_conflict(t, sd, dd); };

    const pd::fs::OpResult r = cut ? pd::fs::move(src, dir_, h) : pd::fs::copy(src, dir_, h);
    ui_.kbd.flush();
    if (cut) {
        // Whatever was moved is gone from its old place; keep the rest.
        std::vector<std::string> left;
        for (const std::string& p : src)
            if (pd::fs::exists(p.c_str()) && !pd::fs::same_path(pd::path::parent(p), dir_)) left.push_back(p);
        clip_.paths = left;
    }
    const std::string first = pd::path::base_name(src[0]);
    report(cut ? "Moved" : "Copied", r);
    refresh_space();
    for (Entry& e : entries_) e.marked = false;
    reload(first);
    draw_all();
}

void FileManager::remove_selected() {
    std::vector<std::string> sel = selection();
    if (sel.empty()) return;
    std::string question;
    if (sel.size() == 1 && !pd::fs::is_dir(sel[0].c_str())) {
        question = "Delete \"" + pd::path::base_name(sel[0]) + "\"?";
    } else {
        ui::busy(ui_, "Delete", "Counting...", true);
        pd::fs::TreeStats st;
        bool stopped = false;
        uint32_t next = 0;
        auto keep = [&]() {
            if (pd::sys::reached(next)) {
                next = pd::sys::now_ms() + 200;
                if (ui_.kbd.esc_pressed()) stopped = true;
            }
            return !stopped;
        };
        for (const std::string& p : sel)
            if (!pd::fs::scan(p, st, keep) && stopped) break;
        if (stopped) {
            draw_all();
            return;
        }
        if (st.dirs == 0) {
            question = "Delete " + plural(st.files, "file", "files") + " (" + size_text(st.bytes) + ")?";
        } else {
            const std::string what = sel.size() == 1 ? "\"" + pd::path::base_name(sel[0]) + "\""
                                                     : plural(uint32_t(sel.size()), "item", "items");
            question = "Delete " + what + " and everything inside? " + plural(st.files, "file", "files") + ", " +
                       plural(st.dirs, "folder", "folders") + ", " + size_text(st.bytes) + ".";
        }
    }
    const bool yes = ui::confirm(ui_, "Delete", question + " This cannot be undone.");
    ui_.kbd.flush();
    if (!yes) {
        draw_all();
        return;
    }
    op_title_ = "Deleting";
    op_last_pmil_ = -1;
    op_force_ = true;
    op_next_esc_ = 0;
    pd::fs::OpHooks h;
    h.progress = [this](uint64_t done, uint64_t total, const std::string& cur) {
        const int pmil = total ? int(std::min<uint64_t>(1000, done * 1000 / total)) : 0;
        if (op_force_ || pmil / 10 != op_last_pmil_ / 10) {
            const int cells = (gfx::width() - 32) / ui_.font().w;
            const std::string label = gfx::clip_cells(pd::path::base_name(cur), cells);
            if (op_force_) ui::progress(ui_, op_title_.c_str(), label, 0, 1000);
            ui::progress(ui_, op_title_.c_str(), label, uint32_t(std::max(1, pmil)), 1000);
            op_force_ = false;
            op_last_pmil_ = pmil;
        }
        if (pd::sys::reached(op_next_esc_)) {
            op_next_esc_ = pd::sys::now_ms() + 200;
            if (ui_.kbd.esc_pressed() && ask_stop()) return false;
        }
        return true;
    };
    const pd::fs::OpResult r = pd::fs::remove_all(sel, h);
    ui_.kbd.flush();
    // Drop deleted items from the clipboard.
    std::vector<std::string> left;
    for (const std::string& p : clip_.paths)
        if (pd::fs::exists(p.c_str())) left.push_back(p);
    clip_.paths = left;
    report("Deleted", r);
    refresh_space();
    for (Entry& e : entries_) e.marked = false;
    reload();
    draw_all();
}

void FileManager::new_folder() {
    std::string name = pd::fs::unique_name(dir_, "New folder", true);
    for (;;) {
        if (!ui::prompt(ui_, "New folder", name, "Name of the folder")) break;
        if (!pd::fs::valid_name(name)) {
            ui::message(ui_, "New folder", {"Not a valid name.", "These are not allowed: \\ / : * ? \" < > |"},
                        ui_.theme().error);
            continue;
        }
        const std::string p = pd::path::join(dir_, name);
        if (pd::fs::exists(p.c_str())) {
            ui::message(ui_, "New folder", {"\"" + name + "\" already exists."}, ui_.theme().error);
            continue;
        }
        if (!pd::fs::mkdir(p.c_str())) {
            ui::message(ui_, "New folder", {"Could not create the folder.", pd::fs::last_error_text()},
                        ui_.theme().error);
            break;
        }
        toast("Created " + name + "/");
        reload(name);
        break;
    }
    ui_.kbd.flush();
    draw_all();
}

void FileManager::rename_selected() {
    const std::string from = current_path();
    if (from.empty()) return;
    const std::string old = pd::path::base_name(from);
    std::string name = old;
    for (;;) {
        if (!ui::prompt(ui_, "Rename", name, nullptr)) break;
        if (name == old) break;
        if (!pd::fs::valid_name(name)) {
            ui::message(ui_, "Rename", {"Not a valid name.", "These are not allowed: \\ / : * ? \" < > |"},
                        ui_.theme().error);
            continue;
        }
        const std::string to = pd::path::join(dir_, name);
        // A change of case only is the same entry on FAT: allowed.
        if (!pd::fs::same_path(from, to) && pd::fs::exists(to.c_str())) {
            ui::message(ui_, "Rename", {"\"" + name + "\" already exists."}, ui_.theme().error);
            continue;
        }
        if (!pd::fs::rename(from.c_str(), to.c_str())) {
            ui::message(ui_, "Rename", {"Could not rename.", pd::fs::last_error_text()}, ui_.theme().error);
            break;
        }
        for (std::string& c : clip_.paths)
            if (pd::fs::same_path(c, from)) c = to;
        reload(name);
        break;
    }
    ui_.kbd.flush();
    draw_all();
}

void FileManager::show_info() {
    std::vector<std::string> sel = selection();
    std::vector<std::string> lines;
    if (sel.empty()) {
        lines.push_back("Folder: " + dir_);
    } else {
        ui::busy(ui_, "Info", "Counting...", true);
        pd::fs::TreeStats st;
        bool stopped = false;
        uint32_t next = 0;
        auto keep = [&]() {
            if (pd::sys::reached(next)) {
                next = pd::sys::now_ms() + 200;
                if (ui_.kbd.esc_pressed()) stopped = true;
            }
            return !stopped;
        };
        for (const std::string& p : sel)
            if (!pd::fs::scan(p, st, keep) && stopped) break;
        if (stopped) {
            draw_all();
            return;
        }
        if (sel.size() == 1) {
            lines.push_back(pd::path::base_name(sel[0]));
            lines.push_back("in " + dir_);
        } else {
            lines.push_back(plural(uint32_t(sel.size()), "item", "items") + " marked in " + dir_);
        }
        const bool single_file = sel.size() == 1 && st.dirs == 0;
        if (single_file) {
            lines.push_back("Size: " + size_text(st.bytes) + " (" + std::to_string(st.bytes) + " bytes)");
        } else {
            // A single folder: do not count the folder itself.
            const uint32_t dirs = sel.size() == 1 && st.dirs ? st.dirs - 1 : st.dirs;
            lines.push_back((sel.size() == 1 ? "Contains " : "") + plural(st.files, "file", "files") + ", " +
                            plural(dirs, "folder", "folders"));
            lines.push_back("Total: " + size_text(st.bytes) + " (" + std::to_string(st.bytes) + " bytes)");
        }
    }
    if (space_known_) lines.push_back("Card: " + size_text(free_) + " free of " + size_text(total_));
    ui::message(ui_, "Info", lines);
    ui_.kbd.flush();
    draw_all();
}

void FileManager::usb_mode() {
    if (!pd::usb::drive_available()) {
        ui::message(ui_, "USB drive", {"Not available in this build.", "It needs a board with native USB."},
                    ui_.theme().error);
        draw_all();
        return;
    }
    save_settings();   // last chance before the computer owns the card
    pd::usb::DriveOptions o;
    o.vendor = "PicoCalc";
    o.product = "SD card";
    o.manufacturer = "ClockworkPi PicoCalc";
    o.usb_product = "PicoCalc SD card";
    o.title = "USB drive";
    const pd::usb::DriveExit r = pd::usb::drive_mode(ui_, o);
    switch (r) {
        case pd::usb::DriveExit::Ejected: toast("Drive ejected - back to the files"); break;
        case pd::usb::DriveExit::Left: toast("USB drive mode ended"); break;
        case pd::usb::DriveExit::NoCard: toast("No SD card"); break;
        case pd::usb::DriveExit::Unavailable: toast("USB drive mode could not start"); break;
    }
    for (Entry& e : entries_) e.marked = false;
    std::vector<std::string> left;
    for (const std::string& p : clip_.paths)
        if (pd::fs::exists(p.c_str())) left.push_back(p);
    clip_.paths = left;
    refresh_space();
    reload();
    draw_all();
}

void FileManager::git_clone() {
    // 1. What to clone.
    std::string url = s_.git_url;
    if (!ui::prompt(ui_, "Git clone", url, "owner/repo, or the repository URL")) {
        draw_all();
        return;
    }
    const std::string norm = pd::git::normalize_url(url);
    if (norm.empty()) {
        ui::message(ui_, "Git clone", {"Not a repository address.", "Examples: octocat/Hello-World",
                                       "https://codeberg.org/owner/repo"}, ui_.theme().error);
        draw_all();
        return;
    }
    s_.git_url = url;
    std::string branch;
    for (;;) {
        draw_all();
        const std::string branch_hint = branch.empty() ? "default" : branch;
        const std::string where = "Into " + gfx::clip_cells(pd::path::join(dir_, pd::git::repo_name(norm)), 28);
        const std::string shown = gfx::clip_cells(norm, 36);   // must outlive the menu
        ui::MenuItem items[] = {
            ui::info(shown.c_str()),
            ui::info(where.c_str()),
            ui::choice("History", &s_.git_full, kHistory, 2),
            ui::action("Branch or tag", branch_hint.c_str()),
            ui::action("Clone"),
        };
        ui::MenuOptions mo;
        mo.initial = 4;
        const int r = ui::menu(ui_, "Git clone", items, mo);
        ui_.kbd.flush();
        if (r == 3) {
            ui::prompt(ui_, "Branch or tag", branch, "empty: the default branch");
            continue;
        }
        if (r != 4) {
            save_settings();
            draw_all();
            return;
        }
        break;
    }
    save_settings();
    draw_all();

    // 2. Network.
    if (!wifi_.ensure(ui_)) {
        draw_all();
        return;
    }
    if (!ca_loaded_) {
        std::string pem;
        if (pd::fs::read_file(kCaPath, pem)) pd::net::tls_add_ca_pem(pem);
        ca_loaded_ = true;
    }

    // 3. Clone, with progress; Esc asks to stop.
    pd::git::CloneOptions o;
    o.url = norm;
    o.parent_dir = dir_;
    o.branch = branch;
    o.depth = s_.git_full ? 0 : 1;
    op_title_ = "Git clone";
    op_force_ = true;
    op_next_esc_ = 0;
    int last_phase = -1;
    uint32_t next_draw = 0;
    pd::git::CloneHooks h;
    h.progress = [&](pd::git::Phase p, uint64_t done, uint64_t total, const std::string& detail) {
        wifi_.used();
        const uint32_t now = pd::sys::now_ms();
        if (op_force_ || int(p) != last_phase || pd::sys::reached(next_draw)) {
            next_draw = now + 150;
            std::string label = pd::git::phase_name(p);
            if (p == pd::git::Phase::Receiving) label += " " + size_text(done);
            else if (p == pd::git::Phase::CheckingOut) label += " " + std::to_string(done) + " files";
            else if (total) label += " " + std::to_string(done) + "/" + std::to_string(total);
            if (p == pd::git::Phase::Connecting && !detail.empty()) label = detail;
            const int cells = (gfx::width() - 32) / ui_.font().w;
            label = gfx::clip_cells(label, cells);
            const uint32_t t = total ? uint32_t(std::min<uint64_t>(1000, done * 1000 / total)) : 0;
            if (op_force_ || int(p) != last_phase) ui::progress(ui_, op_title_.c_str(), label, 0, 1000);
            ui::progress(ui_, op_title_.c_str(), label, std::max<uint32_t>(1, t), 1000);
            op_force_ = false;
            last_phase = int(p);
        }
        if (pd::sys::reached(op_next_esc_)) {
            op_next_esc_ = now + 250;
            if (ui_.kbd.esc_pressed() && ask_stop()) return false;
        }
        return true;
    };
    draw_all();   // clear the Wi-Fi dialogs
    const pd::git::CloneResult r = pd::git::clone(o, h);
    ui_.kbd.flush();
    refresh_space();
    draw_all();
    if (r.ok) {
        std::vector<std::string> lines = {pd::path::base_name(r.dir) + "/", "Branch: " + (r.branch.empty() ? "(detached)" : r.branch),
                                          "Commit: " + r.commit.substr(0, 12),
                                          plural(r.files, "file", "files") + ", " + size_text(r.pack_bytes) + " downloaded"};
        if (r.skipped)
            lines.push_back(plural(r.skipped, "entry", "entries") + " skipped (names the card can't store, submodules)");
        toast("Cloned " + pd::path::base_name(r.dir));
        reload(pd::path::base_name(r.dir));
        draw_all();
        ui::message(ui_, "Cloned", lines);
    } else if (r.cancelled) {
        toast("Clone stopped");
        reload();
    } else {
        ui::message(ui_, "Clone failed", {r.error}, ui_.theme().error);
        reload();
    }
    ui_.kbd.flush();
    draw_all();
}

// ================================================================== Bluetooth
bool FileManager::bt_on() {
    if (pd::bt::powered()) return true;
    if (!pd::bt::available()) {
        ui::message(ui_, "Bluetooth", {"No Bluetooth in this build."}, ui_.theme().error);
        return false;
    }
    pd::bt::set_key_file(kBtKeys);
    pd::bt::set_device_name("PicoCalc");
    ui::busy(ui_, "Bluetooth", "Turning on...", false);
    std::string err;
    if (!pd::bt::start(&err)) {
        ui::message(ui_, "Bluetooth", {"Could not start Bluetooth.", err}, ui_.theme().error);
        pd::bt::stop();
        return false;
    }
    return true;
}

void FileManager::bt_off() { pd::bt::stop(); }

void FileManager::draw_bt_live(const char* title, const std::vector<std::string>& lines) {
    const ui::Theme& t = ui_.theme();
    const gfx::Font& f = gfx::font(gfx::FONT_6x12);
    const int W = gfx::width();
    const int h = int(lines.size() + 1) * (f.h + 2) + 10;
    const int y0 = (gfx::height() - h) / 2;
    gfx::fill(4, y0, W - 8, h, t.panel_bg);
    gfx::frame(4, y0, W - 8, h, t.accent);
    const int cells = (W - 20) / f.w;
    gfx::text(10, y0 + 4, W - 20, f, gfx::pad_cells(title, cells), t.accent, t.panel_bg);
    for (size_t i = 0; i < lines.size(); ++i)
        gfx::text(10, y0 + 4 + int(i + 1) * (f.h + 2), W - 20, f, gfx::pad_cells(gfx::clip_cells(lines[i], cells), cells),
                  t.panel_fg, t.panel_bg);
}

void FileManager::bt_idle() {
    pd::bt::poll();
    // A pairing request can come at any moment (the other device may start
    // it): answer it over whatever dialog is open, then let that dialog redraw.
    if (!in_bt_dialog_ && pd::bt::pair_request(nullptr) && bt_pairing()) {
        draw_all();            // the screen under the dialog...
        ui_.repaint = true;    // ...then the dialog itself
    }
}

bool FileManager::bt_pairing() {
    pd::bt::PairRequest rq;
    if (in_bt_dialog_ || !pd::bt::pair_request(&rq)) return false;
    struct Guard {
        bool& b;
        explicit Guard(bool& x) : b(x) { b = true; }
        ~Guard() { b = false; }
    } guard(in_bt_dialog_);
    std::string text;
    if (rq.just_works) {
        text = "\"" + rq.device + "\" wants to pair. Accept?";
    } else {
        char code[8];
        snprintf(code, sizeof code, "%06u", unsigned(rq.code));
        text = "Code " + std::string(code) + ". Check that \"" + rq.device + "\" shows the same code, then accept.";
    }
    const bool ok = ui::confirm(ui_, "Bluetooth pairing", text);
    ui_.kbd.flush();
    pd::bt::pair_answer(ok);
    return true;
}

std::string FileManager::bt_pick_device() {
    auto pick_from = [&](const std::vector<pd::bt::Device>& devs, bool scanned) -> int {
        std::vector<std::string> labels;
        std::vector<ui::MenuItem> items;
        labels.reserve(devs.size() + 2);
        for (const pd::bt::Device& d : devs)
            labels.push_back(gfx::clip_cells(d.name.empty() ? d.addr : d.name, 26));
        for (size_t i = 0; i < devs.size(); ++i)
            items.push_back(ui::action(labels[i].c_str(), devs[i].paired ? "paired" : devs[i].kind()));
        items.push_back(ui::action(scanned ? "Search again" : "Search for devices"));
        ui::MenuOptions mo;
        mo.initial = devs.empty() ? 0 : 0;
        const int r = ui::menu(ui_, "Send to", items, mo);
        ui_.kbd.flush();
        return r;
    };
    bool scanned = false;
    for (;;) {
        std::vector<pd::bt::Device> devs = pd::bt::devices();
        if (devs.empty() && !scanned) {
            // nothing known yet: search straight away
        } else {
            draw_all();
            const int r = pick_from(devs, scanned);
            if (r < 0) return "";
            if (r < int(devs.size())) return devs[size_t(r)].addr;
        }
        // Search (~10 s); Esc stops it early. Redraw only when the text changes.
        draw_all();
        pd::bt::scan_start();
        scanned = true;
        uint32_t next_esc = 0;
        std::string shown;
        while (pd::bt::scanning()) {
            pd::bt::poll();
            if (bt_pairing()) shown.clear();
            const pd::bt::Diag d = pd::bt::diag();
            char b[64];
            snprintf(b, sizeof b, "Searching... %u found (ev %lu)", unsigned(pd::bt::devices().size()),
                     (unsigned long)d.events);
            if (shown != b) {
                ui::busy(ui_, "Bluetooth", b, true);
                shown = b;
            }
            if (pd::sys::reached(next_esc)) {
                next_esc = pd::sys::now_ms() + 250;
                if (ui_.kbd.esc_pressed()) {
                    pd::bt::scan_stop();
                    break;
                }
            }
            pd::sys::sleep_ms(20);
        }
        pd::bt::scan_stop();
        if (!pd::bt::scan_error().empty()) {
            const pd::bt::Diag d = pd::bt::diag();
            char b[64];
            snprintf(b, sizeof b, "refused: cmd 0x%04X status 0x%02X (%lu)", d.failed_opcode, d.failed_status,
                     (unsigned long)d.failed_count);
            ui::message(ui_, "Bluetooth", {"Search failed:", pd::bt::scan_error(), b, "Please send a photo of this."},
                        ui_.theme().error);
            ui_.kbd.flush();
            draw_all();
            return "";
        }
        if (pd::bt::devices().empty()) {
            ui::message(ui_, "Bluetooth",
                        {"No devices found.", "Android: open Settings > Bluetooth", "(it stays visible while open).",
                         "Ubuntu: open Settings > Bluetooth."});
            ui_.kbd.flush();
            draw_all();
            return "";
        }
    }
}

void FileManager::bt_send(const std::vector<std::string>& sel) {
    std::vector<std::string> files;
    int dirs = 0;
    for (const std::string& p : sel) {
        if (pd::fs::is_dir(p.c_str())) ++dirs;
        else files.push_back(p);
    }
    if (files.empty()) {
        ui::message(ui_, "Send via Bluetooth", {"Mark or highlight files to send.", "(Folders cannot be sent.)"});
        draw_all();
        return;
    }
    if (!bt_on()) {
        draw_all();
        return;
    }
    auto saved_idle = ui_.idle;
    ui_.idle = [this] { bt_idle(); };
    const std::string addr = bt_pick_device();
    if (addr.empty()) {
        ui_.idle = saved_idle;
        bt_off();
        draw_all();
        return;
    }
    draw_all();
    pd::bt::send_start(addr, files);
    uint32_t next_esc = 0, next_draw = 0;
    bool force = true, progress_shown = false, timed_out = false;
    const uint32_t t_start = pd::sys::now_ms();
    std::string live_key;
    pd::bt::SendStatus st;
    for (;;) {
        pd::bt::poll();
        st = pd::bt::send_status();
        if (st.state == pd::bt::SendState::Done || st.state == pd::bt::SendState::Failed ||
            st.state == pd::bt::SendState::Cancelled || st.state == pd::bt::SendState::Idle)
            break;
        if (bt_pairing()) force = true;
        if (pd::sys::reached(next_draw) || force) {
            next_draw = pd::sys::now_ms() + 200;
            if (st.state == pd::bt::SendState::Connecting) {
                // Live view: what the radio is doing, readable in one photo.
                const pd::bt::Diag d = pd::bt::diag();
                const uint32_t secs = (pd::sys::now_ms() - t_start) / 1000;
                std::vector<std::string> lines;
                char b[96];
                snprintf(b, sizeof b, "Connecting to %s  %lus", st.current.device.c_str(), (unsigned long)secs);
                lines.push_back(b);
                snprintf(b, sizeof b, "cmd %lu (last %04X)  ev %lu  data in %lu out %lu", (unsigned long)d.cmds_out,
                         d.last_cmd, (unsigned long)d.events, (unsigned long)d.acl_in, (unsigned long)d.acl_out);
                lines.push_back(b);
                snprintf(b, sizeof b, "sdp %s  goep state %d", d.sdp_idle ? "idle" : "BUSY", d.goep_state);
                lines.push_back(b);
                const size_t from = d.log.size() > 5 ? d.log.size() - 5 : 0;
                for (size_t i = from; i < d.log.size(); ++i) lines.push_back(d.log[i]);
                const size_t sf = d.stack_log.size() > 4 ? d.stack_log.size() - 4 : 0;
                for (size_t i = sf; i < d.stack_log.size(); ++i) lines.push_back("bt: " + d.stack_log[i]);
                // the newest part of the trail, on two lines
                const std::string tr = d.trail.size() > 100 ? d.trail.substr(d.trail.size() - 100) : d.trail;
                lines.push_back(tr.size() > 50 ? tr.substr(0, tr.size() - 50) : "");
                lines.push_back(tr.size() > 50 ? tr.substr(tr.size() - 50) : tr);
                lines.push_back("Esc: stop");
                std::string key;
                for (const std::string& l : lines) key += l;
                if (force || key != live_key) {
                    live_key = key;
                    draw_bt_live("Send via Bluetooth", lines);
                }
                force = false;
                // No progress for a minute: stop and show the log.
                if (secs >= 60) {
                    timed_out = true;
                    pd::bt::send_cancel();
                }
            } else {
                std::string label = std::to_string(st.file_index + 1) + "/" + std::to_string(st.file_count) + " " + st.current.name;
                uint32_t pm = 0;
                if (st.current.total) pm = uint32_t(st.current.done * 1000 / st.current.total);
                const int cells = (gfx::width() - 32) / ui_.font().w;
                label = gfx::clip_cells(label, cells);
                if (force || !progress_shown) {
                    draw_all();
                    ui::progress(ui_, "Send via Bluetooth", label, 0, 1000);
                    progress_shown = true;
                }
                ui::progress(ui_, "Send via Bluetooth", label, std::max<uint32_t>(1, pm), 1000);
                force = false;
            }
        }
        if (pd::sys::reached(next_esc)) {
            next_esc = pd::sys::now_ms() + 250;
            if (ui_.kbd.esc_pressed()) {
                op_title_ = "Send via Bluetooth";
                if (ask_stop()) pd::bt::send_cancel();
                force = true;
            }
        }
        pd::sys::sleep_ms(2);
    }
    ui_.idle = saved_idle;
    bt_log_tail_ = pd::bt::diag().log;
    bt_trail_ = pd::bt::diag().trail;
    bt_stack_log_ = pd::bt::diag().stack_log;
    bt_off();
    ui_.kbd.flush();
    draw_all();
    if (st.state == pd::bt::SendState::Done && st.error.empty()) {
        toast("Sent " + plural(st.sent_ok, "file", "files") + " to " + st.current.device);
    } else if (st.state == pd::bt::SendState::Cancelled && st.sent_ok > 0) {
        toast("Sending stopped");
    } else {
        // Failed, or stopped before anything went through: show what happened.
        std::vector<std::string> lines = {plural(st.sent_ok, "file", "files") + " of " + std::to_string(st.file_count) + " sent." +
                                          (st.state == pd::bt::SendState::Cancelled ? " (stopped)" : "")};
        if (timed_out) lines.push_back("No connection after 60 seconds.");
        if (!st.error.empty()) lines.push_back(st.error);
        lines.push_back("trail (newest last):");
        lines.push_back(bt_trail_.size() > 50 ? bt_trail_.substr(bt_trail_.size() - 50) : bt_trail_);
        for (const std::string& l : bt_stack_log_) lines.push_back("bt: " + l);
        lines.push_back("Recent Bluetooth events:");
        const std::vector<std::string> log = bt_log_tail_;
        for (const std::string& l : log) lines.push_back(l);
        lines.push_back("Please send a photo of this.");
        ui::text_view(ui_, "Send via Bluetooth", lines);
        ui_.kbd.flush();
        draw_all();
    }
    if (dirs) toast(plural(uint32_t(dirs), "folder", "folders") + " skipped (only files can be sent)");
}

void FileManager::bt_receive() {
    if (!bt_on()) {
        draw_all();
        return;
    }
    auto saved_idle = ui_.idle;
    ui_.idle = [this] { bt_idle(); };
    pd::bt::receive_start(dir_);
    const ui::Theme& t = ui_.theme();
    const gfx::Font& big = gfx::font(gfx::FONT_8x16);
    const gfx::Font& sm = gfx::font(gfx::FONT_6x12);
    const int W = gfx::width();
    auto draw_static = [&]() {
        gfx::clear(t.bg);
        bar_.draw(t, "Bluetooth", "", true);
        int y = bar_.height() + 8;
        gfx::text_centered(y, big, "Receiving files", t.accent, t.bg);
        y += big.h + 4;
        const std::string into = "into " + clip_left(dir_, (W - 16) / sm.w - 5);
        gfx::text_centered(y, sm, into.c_str(), t.fg, t.bg);
        y += sm.h + 2;
        const std::string vis = "Visible as \"PicoCalc " + pd::bt::address().substr(9) + "\"";
        gfx::text_centered(y, sm, vis.c_str(), t.fg, t.bg);
        y += sm.h + 8;
        const char* help[] = {"Android: Share > Bluetooth > PicoCalc", "Ubuntu: Settings > Bluetooth > PicoCalc",
                              "        > Send Files"};
        for (const char* h : help) {
            gfx::text_centered(y, sm, h, t.dim, t.bg);
            y += sm.h + 2;
        }
        gfx::text_centered(gfx::height() - sm.h - 4, sm, "Esc: stop receiving", t.dim, t.bg);
    };
    const int y_state = bar_.height() + 8 + big.h + 4 + 2 * (sm.h + 2) + 6 + 3 * (sm.h + 2) + 8;
    draw_static();
    ui_.kbd.flush();
    std::string last;
    uint32_t next_draw = 0, next_keys = 0;
    size_t shown_received = 0;
    for (;;) {
        pd::bt::poll();
        if (bt_pairing()) {
            draw_static();
            last.clear();
        }
        const pd::bt::ReceiveStatus st = pd::bt::receive_status();
        if (pd::sys::reached(next_draw)) {
            next_draw = pd::sys::now_ms() + 250;
            std::string state;
            if (st.current.active) {
                state = "Receiving " + st.current.name;
                if (st.current.total) state += " " + std::to_string(st.current.done * 100 / st.current.total) + "%";
                else state += " " + size_text(st.current.done);
            } else {
                state = st.connected ? "Connected to " + st.current.device : "Waiting for a device...";
            }
            const pd::bt::Diag d = pd::bt::diag();
            std::vector<std::string> info;
            char b[96];
            snprintf(b, sizeof b, "cmd %lu  ev %lu  data in %lu out %lu  %s", (unsigned long)d.cmds_out, (unsigned long)d.events,
                     (unsigned long)d.acl_in, (unsigned long)d.acl_out, d.discoverable ? "visible" : "HIDDEN");
            info.push_back(b);
            const size_t from = d.log.size() > 6 ? d.log.size() - 6 : 0;
            for (size_t i = from; i < d.log.size(); ++i) info.push_back(d.log[i]);
            const size_t sf = d.stack_log.size() > 2 ? d.stack_log.size() - 2 : 0;
            for (size_t i = sf; i < d.stack_log.size(); ++i) info.push_back("bt: " + d.stack_log[i]);
            info.push_back(d.trail.size() > 50 ? d.trail.substr(d.trail.size() - 50) : d.trail);
            std::string all = state + "|" + std::to_string(st.received.size()) + st.error;
            for (const std::string& l : info) all += l;
            if (all != last) {
                last = all;
                int y = y_state;
                gfx::fill(0, y, W, gfx::height() - sm.h - 6 - y, t.bg);
                gfx::text_centered(y, big, gfx::clip_cells(state, W / big.w - 1).c_str(),
                                   st.current.active ? t.accent : t.fg, t.bg);
                y += big.h + 2;
                const std::string n = st.received.empty() ? "No files yet"
                                                          : plural(uint32_t(st.received.size()), "file", "files") + " received, last: " +
                                                                pd::path::base_name(st.received.back());
                gfx::text(4, y, W - 8, sm, gfx::clip_cells(n, W / sm.w - 1), t.accent, t.bg);
                y += sm.h + 1;
                if (!st.error.empty()) {
                    gfx::text(4, y, W - 8, sm, gfx::clip_cells(st.error, W / sm.w - 1), t.error, t.bg);
                    y += sm.h + 1;
                }
                for (const std::string& l : info) {
                    if (y + sm.h > gfx::height() - sm.h - 6) break;
                    gfx::text(4, y, W - 8, sm, gfx::clip_cells(l, W / sm.w - 1), t.dim, t.bg);
                    y += sm.h + 1;
                }
            }
            if (st.received.size() != shown_received) shown_received = st.received.size();
            bar_.draw(t, "Bluetooth", "");
        }
        if (pd::sys::reached(next_keys)) {
            next_keys = pd::sys::now_ms() + (st.current.active ? 300 : 60);
            pd::input::KeyEvent ev;
            if (ui_.kbd.poll(ev) && ev.code == key::Esc) {
                if (!st.current.active) break;
                op_title_ = "Bluetooth";
                if (ask_stop()) break;
                draw_static();
                last.clear();
            }
        }
        pd::sys::sleep_ms(2);
    }
    const size_t got = pd::bt::receive_status().received.size();
    pd::bt::receive_stop();
    ui_.idle = saved_idle;
    bt_off();
    ui_.kbd.flush();
    refresh_space();
    reload();
    draw_all();
    toast(got ? "Received " + plural(uint32_t(got), "file", "files") : "Bluetooth off");
}

void FileManager::bt_menu() {
    const std::vector<std::string> sel = selection();
    const std::string send_label = sel.empty() ? "Send files" : "Send " + plural(uint32_t(sel.size()), "item", "items");
    ui::MenuItem items[] = {
        ui::action(send_label.c_str()),
        ui::action("Receive files here"),
        ui::action("Paired devices..."),
        ui::action("Diagnostics"),
    };
    items[0].enabled = !sel.empty();
    ui::MenuOptions mo;
    mo.initial = sel.empty() ? 1 : 0;
    const int r = ui::menu(ui_, "Bluetooth", items, mo);
    ui_.kbd.flush();
    if (r == 0) {
        bt_send(sel);
        for (Entry& e : entries_) e.marked = false;
        dirty_all_ = true;
        return;
    }
    if (r == 1) {
        bt_receive();
        return;
    }
    if (r == 3) {
        if (bt_on()) {
            auto saved_idle = ui_.idle;
            ui_.idle = [this] { bt_idle(); };
            // Let the radio settle, then show what it reports.
            const uint32_t until = pd::sys::now_ms() + 500;
            while (!pd::sys::reached(until)) pd::bt::poll();
            const pd::bt::Diag d = pd::bt::diag();
            char l1[64], l2[64], l3[64], l4[64];
            snprintf(l1, sizeof l1, "HCI %s, address %s", d.hci_on ? "on" : "off", pd::bt::address().c_str());
            snprintf(l2, sizeof l2, "events %lu, data packets %lu", (unsigned long)d.events, (unsigned long)d.acl_packets);
            snprintf(l3, sizeof l3, "refused commands %lu (last 0x%04X st 0x%02X)", (unsigned long)d.failed_count,
                     d.failed_opcode, d.failed_status);
            snprintf(l4, sizeof l4, "last search: %lu results", (unsigned long)d.inquiry_results);
            char l5[96];
            snprintf(l5, sizeof l5, "cmd %lu (last %04X)  data in %lu out %lu  sdp %s", (unsigned long)d.cmds_out,
                     d.last_cmd, (unsigned long)d.acl_in, (unsigned long)d.acl_out, d.sdp_idle ? "idle" : "BUSY");
            std::vector<std::string> lines = {l1, l2, l3, l4, l5, "trail: " + d.trail, "", "Event log:"};
            for (const std::string& l : d.log) lines.push_back(l);
            lines.push_back("");
            lines.push_back("BTstack messages:");
            for (const std::string& l : d.stack_log) lines.push_back(l);
            ui::text_view(ui_, "Bluetooth diagnostics", lines);
            ui_.kbd.flush();
            ui_.idle = saved_idle;
            bt_off();
        }
        draw_all();
        return;
    }
    if (r == 2 && bt_on()) {
        auto saved_idle = ui_.idle;
        ui_.idle = [this] { bt_idle(); };
        for (;;) {
            std::vector<pd::bt::Device> devs;
            for (const pd::bt::Device& d : pd::bt::devices())
                if (d.paired) devs.push_back(d);
            if (devs.empty()) {
                ui::message(ui_, "Paired devices", {"No paired devices.", "Pairing happens on the first transfer."});
                break;
            }
            std::vector<std::string> labels;
            std::vector<ui::MenuItem> it;
            for (const pd::bt::Device& d : devs) labels.push_back(gfx::clip_cells(d.name.empty() ? d.addr : d.name, 26));
            for (size_t i = 0; i < devs.size(); ++i) it.push_back(ui::action(labels[i].c_str(), "forget"));
            draw_all();
            const int k = ui::menu(ui_, "Paired devices", it);
            ui_.kbd.flush();
            if (k < 0) break;
            if (ui::confirm(ui_, "Forget device", "Forget \"" + labels[size_t(k)] + "\"? It will ask to pair again."))
                pd::bt::forget(devs[size_t(k)].addr);
            ui_.kbd.flush();
        }
        ui_.idle = saved_idle;
        bt_off();
    }
    draw_all();
}

void FileManager::file_actions() {
    const std::string p = current_path();
    if (p.empty()) return;
    const std::string title = gfx::clip_cells(pd::path::base_name(p), 30);
    ui::MenuItem items[] = {
        ui::action("Copy", "c"),   ui::action("Cut", "x"),    ui::action("Rename", "r"),
        ui::action("Delete", "d"), ui::action("Info", "i"),    ui::action("Send via Bluetooth", "b"),
    };
    const int r = ui::menu(ui_, title.c_str(), items);
    ui_.kbd.flush();
    switch (r) {
        case 0: to_clipboard(false); break;
        case 1: to_clipboard(true); break;
        case 2: rename_selected(); return;
        case 3: remove_selected(); return;
        case 4: show_info(); return;
        case 5: bt_send(selection()); return;
        default: break;
    }
    draw_all();
}

void FileManager::help() {
    ui::text_view(ui_, "Keys",
                  {
                      "Up/Down PgUp/PgDn Home/End  move",
                      "Enter or Right   open folder",
                      "                 (on a file: actions)",
                      "Bksp or Left     parent folder",
                      "Space            mark / unmark",
                      "a                mark all / none",
                      "",
                      "c  Ctrl+C        copy",
                      "x  Ctrl+X        cut",
                      "v  Ctrl+V        paste here",
                      "d  Del           delete",
                      "r  F2            rename",
                      "n  F7            new folder",
                      "i                size and info",
                      "b                Bluetooth send / receive",
                      "g                git clone (Wi-Fi)",
                      "u                USB drive mode",
                      "h                show hidden files",
                      "s                change sort order",
                      "Esc              menu",
                      "",
                      "Marked entries are used by copy, cut,",
                      "delete and info; with nothing marked",
                      "the highlighted entry is used.",
                      "",
                      "USB drive mode: connect the Pico's",
                      "micro-USB port to a computer. The SD",
                      "card appears as a USB drive. Eject it",
                      "on the computer to come back here.",
                  });
    ui_.kbd.flush();
    draw_all();
}

void FileManager::about() {
    ui::message(ui_, "About",
                {std::string("Pod Files ") + PD_APP_VERSION, "A file manager built on PicoPods.",
                 std::string("Board: ") + pd::board::name() + " (" + pd::board::chip() + ")",
                 std::string("Terminal: ") + term_.name(),
                 space_known_ ? "Card: " + size_text(total_) + ", " + size_text(free_) + " free" : "Card: ?"});
    ui_.kbd.flush();
    draw_all();
}

void FileManager::main_menu() {
    const bool has_sel = !selection().empty();
    enum {
        kPaste, kCopy, kCut, kDelete, kRename, kNewFolder, kMarkAll, kInfo, kBt, kGit, kWifi, kUsb, kHidden, kSort,
        kFont, kTheme, kHelp, kAbout
    };
    const std::string wifi_hint = pd::net::connected() ? "on" : "off";
    const std::string paste_hint = clip_.empty() ? "v" : std::to_string(clip_.paths.size()) + " - v";
    std::vector<ui::MenuItem> items = {
        ui::action("Paste here", paste_hint.c_str()),
        ui::action("Copy", "c"),
        ui::action("Cut", "x"),
        ui::action("Delete", "d"),
        ui::action("Rename", "r"),
        ui::action("New folder", "n"),
        ui::action("Mark all / none", "a"),
        ui::action("Info", "i"),
        ui::action("Bluetooth...", "b"),
        ui::action("Git clone...", "g"),
        ui::action("Wi-Fi...", wifi_hint.c_str()),
        ui::action("USB drive mode", "u"),
        ui::choice("Hidden files", &s_.show_hidden, kOnOff, 2),
        ui::choice("Sort by", &s_.sort, kSortNames, 3),
        ui::choice("List font", &s_.font, kFontNames, 2),
        ui::choice("Theme", &s_.theme, kThemeNames, 3),
        ui::action("Help", "F1"),
        ui::action("About"),
    };
    items[kPaste].enabled = !clip_.empty();
    items[kCopy].enabled = items[kCut].enabled = items[kDelete].enabled = has_sel;
    items[kRename].enabled = !current_path().empty();
    items[kUsb].enabled = pd::usb::drive_available();
    const int hidden = s_.show_hidden, sort = s_.sort, font = s_.font;
    ui::MenuOptions mo;
    mo.initial = clip_.empty() ? 1 : 0;
    mo.on_change = [this](int i) {
        if (i == kTheme) apply_theme();
    };
    const int r = ui::menu(ui_, "MENU", items, mo);
    ui_.kbd.flush();
    const bool relist = hidden != s_.show_hidden || sort != s_.sort;
    if (relist || font != s_.font || r == kTheme) {
        save_settings();
        if (relist) reload();
    }
    switch (r) {
        case kPaste: paste(); return;
        case kCopy: to_clipboard(false); break;
        case kCut: to_clipboard(true); break;
        case kDelete: remove_selected(); return;
        case kRename: rename_selected(); return;
        case kNewFolder: new_folder(); return;
        case kMarkAll: mark_all(); break;
        case kInfo: show_info(); return;
        case kUsb: usb_mode(); return;
        case kGit: git_clone(); return;
        case kBt: bt_menu(); return;
        case kWifi:
            wifi_.menu(ui_);
            ui_.kbd.flush();
            draw_all();
            return;
        case kHelp: help(); return;
        case kAbout: about(); return;
        default: break;
    }
    save_settings();
    draw_all();
}

// ================================================================== main loop
void FileManager::run() {
    load_settings();
    wifi_.load();
    apply_theme();
    dir_ = s_.dir;
    refresh_space();
    reload();
    draw_all();

    for (;;) {
        pd::input::KeyEvent ev;
        if (!term_.keyboard().poll(ev)) {
            if (dirty_all_) {
                draw_header();
                draw_rows();
                dirty_all_ = false;
                draw_status();
                dirty_footer_ = true;
            }
            // Remember the folder across restarts, once browsing pauses.
            if (dir_ != s_.dir && pd::sys::reached(dir_save_at_)) {
                s_.dir = dir_;
                save_settings();
            }
            if (dirty_footer_ || (!toast_.empty() && pd::sys::reached(toast_until_))) {
                if (pd::sys::reached(toast_until_)) toast_.clear();
                draw_footer();
                dirty_footer_ = false;
            }
            if (wifi_.tick()) toast("Wi-Fi turned off (idle)");
            draw_status();   // battery changes
            pd::sys::sleep_ms(5);
            continue;
        }
        const int n = int(entries_.size());
        const uint8_t c = ev.ctrl ? key::lower(ev.code) : ev.code;
        if (ev.ctrl) {
            if (c == 'c') to_clipboard(false);
            else if (c == 'x') to_clipboard(true);
            else if (c == 'v') paste();
            else if (c == 'a') mark_all();
            continue;
        }
        switch (c) {
            case key::Up: if (n) sel_ = (sel_ + n - 1) % n; draw_rows(); break;
            case key::Down: if (n) sel_ = (sel_ + 1) % n; draw_rows(); break;
            case key::PageUp: sel_ = std::max(0, sel_ - rows()); draw_rows(); break;
            case key::PageDown: sel_ = std::max(0, std::min(n - 1, sel_ + rows())); draw_rows(); break;
            case key::Home: sel_ = 0; draw_rows(); break;
            case key::End: sel_ = std::max(0, n - 1); draw_rows(); break;
            case key::Enter:
            case key::Right: open_selected(); break;
            case key::Backspace:
            case key::Left: go_up(); break;
            case key::Space: toggle_mark(); break;
            case 'a': case 'A': mark_all(); break;
            case 'c': case 'C': to_clipboard(false); break;
            case 'x': case 'X': to_clipboard(true); break;
            case 'v': case 'V': paste(); break;
            case 'd': case 'D': case key::Del: remove_selected(); break;
            case 'r': case 'R': case key::F2: rename_selected(); break;
            case 'n': case 'N': case key::F7: new_folder(); break;
            case 'i': case 'I': show_info(); break;
            case 'u': case 'U': usb_mode(); break;
            case 'g': case 'G': git_clone(); break;
            case 'b': case 'B': bt_menu(); break;
            case 'h': case 'H': case '.':
                s_.show_hidden = !s_.show_hidden;
                toast(s_.show_hidden ? "Showing hidden files" : "Hiding hidden files");
                reload();
                save_settings();
                break;
            case 's': case 'S':
                s_.sort = (s_.sort + 1) % 3;
                toast(std::string("Sorted by ") + kSortNames[s_.sort]);
                reload();
                save_settings();
                break;
            case key::F1: case '?': help(); break;
            case key::Esc: main_menu(); break;
            default: break;
        }
        dir_save_at_ = pd::sys::now_ms() + 3000;
    }
}

// Pod Files was PicoCalc Files: take over its settings, Wi-Fi credentials
// and Bluetooth pairings once, by renaming the old folder.
void migrate_work_dir() {
    if (!pd::fs::exists(kWorkDir) && pd::fs::is_dir(kOldWorkDir)) pd::fs::rename(kOldWorkDir, kWorkDir);
}

}  // namespace

int main() {
    pd::diag::begin("Pod Files", PD_APP_VERSION, kStages, pd::array_count(kStages));
    pd::diag::stage(kStart);

    pd::TerminalConfig cfg;
    cfg.audio = false;
    pd::Terminal& term = pd::terminal_init(cfg);
    pd::gfx::init(term.display());
    pd::diag::display_ready();
    pd::diag::stage(kDisplay);

    ui::Context ui(term.keyboard(), ui::THEME_NIGHT);

    pd::diag::stage(kSd);
    pd::app::mount_storage(ui);

    pd::diag::stage(kSettings);
    migrate_work_dir();
    FileManager fm(term, ui);

    pd::diag::stage(kReady);
    pd::diag::done();
    fm.run();
    return 0;
}
