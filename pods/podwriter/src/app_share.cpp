// ePub export, Wi-Fi and sending books to an Xteink X3 running CrossPoint.
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "app.h"
#include "pd/crosspoint.h"
#include "pd/epub.h"
#include "pd/fs.h"

namespace {
uint32_t now() { return pd::sys::now_ms(); }

std::string stem(const std::string& name) {
    const size_t dot = name.rfind('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}
}  // namespace

// Shows `what` in the status line; returns a callback that reports false
// once Esc is pressed.
net::KeepGoing App::escChecker(const char* what) {
    msg_ = std::string(what) + "  (Esc cancels)";
    msgUntil_ = now() + 120000;
    renderStatus(true);
    return [this] {
        KeyEvent ev;
        return !(kbd_.poll(ev) && ev.code == key::Esc);
    };
}

// ---------------------------------------------------------------------------
// ePub export
// ---------------------------------------------------------------------------

bool App::exportEpub(std::string* outPath) {
    const std::string docPath = tb_.path().empty() ? "untitled.txt" : tb_.path();
    epub::Options o;
    o.kind = epub::kindForPath(docPath);
    o.author = set_.author;
    o.language = set_.lang.empty() ? "en" : set_.lang;
    o.generator = "Pod Writer";
    const epub::Reader rd = [this](uint32_t pos, uint8_t* buf, uint32_t n) { return tb_.read(pos, buf, n); };
    std::string title = epub::guessTitle(rd, tb_.size(), o.kind, stem(docName()));
    const bool ok = pd::ui::prompt(ui_, "ePub title", title, "Enter = export   Esc = cancel");
    relayout();
    if (!ok) return false;
    o.title = title.empty() ? stem(docName()) : title;

    const std::string dir = tb_.path().empty() ? set_.cwd : pd::path::parent(tb_.path());
    const std::string name = epub::safeFileName(tb_.path().empty() ? o.title : stem(docName())) + ".epub";
    const std::string out = pd::path::join(dir, name);
    std::string err;
    const bool written = epub::write(rd, tb_.size(), o, out, &err, progressFn("Exporting", true));
    msg_.clear();
    if (!written) {
        pd::ui::message(ui_, "Export failed", {err});
        relayout();
        return false;
    }
    *outPath = out;
    return true;
}

void App::cmdExportEpub() {
    std::string out;
    if (exportEpub(&out)) setMessage("Exported " + out, 4000);
}

// ---------------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------------

// Wi-Fi comes from PicoPods' pd::WifiManager (credentials in /.fwedit/wifi.txt,
// network picker, idle power-off). These wrappers repaint the editor after it.
void App::wifiUsed() { wifi_.used(); }

bool App::ensureWifi() {
    const bool ok = wifi_.ensure(ui_);
    relayout();
    statusCache_.clear();
    if (ok) setMessage("Connected to " + wifi_.ssid() + "  " + net::ip_address(), 4000);
    return ok;
}

void App::cmdWifi() {
    wifi_.menu(ui_);
    relayout();
    statusCache_.clear();
}

void App::checkWifiIdle() {
    if (wifi_.tick()) {
        statusCache_.clear();
        setMessage("Wi-Fi turned off (idle)");
    }
}

// ---------------------------------------------------------------------------
// X3 (CrossPoint)
// ---------------------------------------------------------------------------

namespace {
// Finds the reader: its host name first (mDNS for *.local), then the last
// address it answered from.
bool findReader(const x3::Target& t, const std::string& lastIp, uint32_t* ip, std::string* err,
                const net::KeepGoing& kg) {
    if (net::resolve(t.host, ip, 6000, kg, err)) return true;
    if (*err == "Cancelled" || lastIp.empty()) return false;
    std::string e2;
    return net::resolve(lastIp, ip, 1000, kg, &e2);
}

std::vector<std::string> unreachable(const std::string& url, const std::string& err) {
    return {"Cannot reach the X3 at " + url + " (" + err + ").",
            "On the X3 open File Transfer and join the same Wi-Fi as the PicoCalc.",
            "If it still fails, set the IP shown on the X3 in ePub & X3 settings."};
}
}  // namespace

void App::cmdSendToX3() {
    x3::Target target;
    std::string err;
    if (!x3::parse_url(set_.x3Url, &target, &err)) {
        pd::ui::message(ui_, "Send to X3", {"Bad X3 address: " + err, "Fix it in ePub & X3 settings."});
        relayout();
        return;
    }
    std::string path;
    if (!exportEpub(&path)) return;
    if (!ensureWifi()) {
        setMessage("ePub saved to " + path + " (not sent)", 4000);
        return;
    }
    uint32_t ip = 0;
    const bool found = findReader(target, set_.x3Ip, &ip, &err, escChecker("Looking for the X3..."));
    msg_.clear();
    if (!found) {
        if (err != "Cancelled") pd::ui::message(ui_, "Send to X3", unreachable(set_.x3Url, err));
        relayout();
        return;
    }

    fs::File f;
    if (!f.open(path.c_str(), fs::Mode::Read)) {
        pd::ui::message(ui_, "Send to X3", {"Cannot reopen " + path});
        relayout();
        return;
    }
    const uint32_t size = f.size();
    const x3::Source src = [&f](uint32_t off, uint8_t* buf, uint32_t n) -> int {
        if (!f.seek(off)) return -1;
        return f.read(buf, n);
    };
    const std::string name = pd::path::base_name(path);
    std::string dir = set_.x3Dir.empty() ? "/" : set_.x3Dir;
    if (dir[0] != '/') dir = "/" + dir;
    x3::Client client(target, ip);
    x3::Reply reply;
    bool ok = client.upload(dir, name, size, src, &reply, progressFn("Sending", true), &err);
    if (ok && x3::is_already_exists(reply)) {
        msg_.clear();
        const int r = pd::ui::ask(ui_, "Send to X3", name + " is already on the X3. Replace it?", false);
        relayout();
        if (r != 1) {
            setMessage("Not sent: already on the X3");
            return;
        }
        const std::string remote = (dir == "/" ? "" : dir) + "/" + name;
        if (!client.remove(remote, &reply, escChecker("Replacing..."), &err)) ok = false;
        else ok = client.upload(dir, name, size, src, &reply, progressFn("Sending", true), &err);
    }
    msg_.clear();
    wifiUsed();
    if (!ok) {
        if (err == "Cancelled") setMessage("Sending cancelled");
        else pd::ui::message(ui_, "Send to X3", unreachable(set_.x3Url, err));
        relayout();
        return;
    }
    if (reply.status != 200) {
        char s[16];
        snprintf(s, sizeof s, "%d", reply.status);
        pd::ui::message(ui_, "Send to X3", {std::string("The X3 answered ") + s + ":", reply.body});
        relayout();
        return;
    }
    const std::string ipStr = net::ip_to_string(ip);
    if (ipStr != set_.x3Ip) {
        set_.x3Ip = ipStr;
        saveSettings();
    }
    setMessage("Sent to X3: " + (dir == "/" ? "" : dir) + "/" + name, 5000);
}

void App::cmdShareSettings() {
    int sel = 0;
    for (;;) {
        const std::string a = "Author: " + (set_.author.empty() ? std::string("-") : set_.author);
        const std::string l = "Book language: " + set_.lang;
        const std::string u = "X3 address: " + set_.x3Url;
        const std::string d = "X3 folder: " + set_.x3Dir;
        const std::vector<pd::ui::MenuItem> items = {
            pd::ui::action(a.c_str()), pd::ui::action(l.c_str()), pd::ui::action(u.c_str()),
            pd::ui::action(d.c_str()), pd::ui::action("Test X3 connection"),
        };
        pd::ui::MenuOptions opt;
        opt.initial = sel;
        sel = pd::ui::menu(ui_, "ePub & X3 settings", items, opt);
        relayout();
        if (sel < 0) return;
        std::string v;
        switch (sel) {
            case 0:
                v = set_.author;
                if (pd::ui::prompt(ui_, "Author (ePub metadata)", v)) set_.author = v;
                break;
            case 1:
                v = set_.lang;
                if (pd::ui::prompt(ui_, "Book language (en, it, de...)", v) && !v.empty()) set_.lang = v;
                break;
            case 2:
                v = set_.x3Url;
                if (pd::ui::prompt(ui_, "X3 address (URL or IP)", v, "Empty = http://crosspoint.local/")) {
                    x3::Target t;
                    std::string err;
                    if (v.empty()) v = x3::kDefaultUrl;
                    if (x3::parse_url(v, &t, &err)) {
                        set_.x3Url = v;
                        set_.x3Ip.clear();
                    } else {
                        pd::ui::message(ui_, "X3 address", {err});
                    }
                }
                break;
            case 3:
                v = set_.x3Dir;
                if (pd::ui::prompt(ui_, "Folder on the X3", v, "e.g. / or /Books")) set_.x3Dir = v.empty() ? "/" : v;
                break;
            case 4: {
                x3::Target t;
                std::string err;
                std::vector<std::string> lines;
                uint32_t ip = 0;
                x3::Reply r;
                if (!x3::parse_url(set_.x3Url, &t, &err)) {
                    lines = {"Bad address: " + err};
                } else if (!ensureWifi()) {
                    break;
                } else if (!findReader(t, set_.x3Ip, &ip, &err, escChecker("Looking for the X3..."))) {
                    lines = unreachable(set_.x3Url, err);
                } else if (!x3::Client(t, ip).status(&r, escChecker("Asking the X3..."), &err) ||
                           r.status != 200) {
                    lines = unreachable(set_.x3Url, err.empty() ? "no answer" : err);
                } else {
                    lines = {"Found at " + net::ip_to_string(ip) + ":", r.body};
                    if (set_.x3Ip != net::ip_to_string(ip)) set_.x3Ip = net::ip_to_string(ip);
                }
                msg_.clear();
                wifiUsed();
                pd::ui::message(ui_, "Test X3 connection", lines);
                break;
            }
        }
        saveSettings();
        relayout();
    }
}
