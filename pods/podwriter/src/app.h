// Pod Writer - the editor application: state, rendering and commands.
// Built on the PicoPods libraries: the editor engine (pd_text), display and fonts (pd_gfx),
// dialogs (pd_ui), keyboard (pd_input), files (pd_fs), Wi-Fi (pd_wifi),
// ePub (pd_epub) and the X3 client (pd_crosspoint).
#pragma once
#include <cstdint>
#include <string>

#include "pd/crosspoint.h"
#include "pd/epub.h"
#include "pd/net.h"
#include "pd/pd.h"
#include "pd/text/layout.h"
#include "pd/text/palette.h"
#include "pd/text/syntax.h"
#include "pd/text/text_buffer.h"
#include "pd/wifi.h"

// Short names for the PicoPods types the editor uses everywhere.
using pd::text::Layout;
using pd::text::Row;
using pd::text::TextBuffer;
using KeyEvent = pd::input::KeyEvent;
using Keyboard = pd::input::Keyboard;
using Font = pd::gfx::Font;
using Cell = pd::gfx::Cell;
using Theme = pd::ui::Theme;
namespace syn = pd::syn;
namespace key = pd::key;
namespace fs = pd::fs;
namespace net = pd::net;
namespace epub = pd::epub;
namespace x3 = pd::crosspoint;

#define FW_VERSION PD_APP_VERSION

class App {
public:
    explicit App(Keyboard& kbd);
    void run();

private:
    static constexpr uint32_t kNone = UINT32_MAX;
    static constexpr int kMaxRows = 32;
    static constexpr int kStatusH = 14;

    struct Settings {
        int font = 1;
        int theme = 0;
        bool status = true;
        bool syntax = true;
        std::string file;
        std::string cwd = "/";
        uint32_t pos = 0;
        // ePub export and the X3 reader
        std::string author;
        std::string lang = "en";
        std::string x3Url = "http://crosspoint.local/";
        std::string x3Dir = "/";
        std::string x3Ip;  // last address the reader was found at
    };

    // setup / settings
    void waitForCard();
    void loadSettings();
    void saveSettings();

    // geometry & rendering
    void relayout();
    void invalidate();
    void render();
    void renderStatus(bool force);
    syn::Lang docLang() const;
    void synPrepare(syn::Lang lang);            // puts lex_ in the state at top_
    void synFeed(uint32_t from, uint32_t to);   // advances lex_ over [from, to)
    void cmdToggleSyntax();
    void applySyntax(bool on);

    // ePub export, Wi-Fi and the X3 reader (app_share.cpp)
    bool exportEpub(std::string* outPath);
    void cmdExportEpub();
    void cmdSendToX3();
    void cmdWifi();
    void cmdShareSettings();
    bool ensureWifi();
    void wifiUsed();
    void checkWifiIdle();
    net::KeepGoing escChecker(const char* what);
    void setMessage(const std::string& m, uint32_t ms = 2500);
    void showProgress(const char* what, uint32_t done, uint32_t total);
    TextBuffer::Progress progressFn(const char* what, bool cancellable);
    const Theme& th() const { return pd::ui::theme(set_.theme); }

    // cursor / view
    bool hasSel() const { return anchor_ != kNone && anchor_ != cursor_; }
    void selRange(uint32_t* a, uint32_t* b) const;
    void moveTo(uint32_t pos, bool extend);
    uint32_t vertical(uint32_t pos, int dir);
    void ensureVisible();
    void centerCursor();
    void normalizeTop();
    uint32_t wordLeft(uint32_t p);
    uint32_t wordRight(uint32_t p);

    // editing
    void handleKey(const KeyEvent& ev);
    void typeText(const char* s, uint32_t n);
    bool deleteSelection();
    void afterEdit();
    void editFailed();

    // commands
    void cmdMenu();
    void cmdNew();
    void cmdOpen();
    bool cmdSave();
    bool cmdSaveAs();
    bool saveTo(const std::string& path);
    bool maybeSaveChanges();
    void cmdFind();
    void cmdFindNext();
    void cmdReplace();
    void cmdGoto();
    void cmdStats();
    void cmdUndo(bool redo);
    void cmdCopy(bool cut);
    void cmdPaste();
    void cmdRestart();
    void cmdDisplayTest();
    void showHelp();
    std::string docName() const;

    Keyboard& kbd_;
    pd::ui::Context ui_;
    pd::WifiManager wifi_{"/.fwedit/wifi.txt"};
    TextBuffer tb_;
    Layout lay_;
    Settings set_;

    uint32_t cursor_ = 0, anchor_ = kNone, top_ = 0;
    int goal_ = -1;
    bool overwrite_ = false;

    const Font* font_ = nullptr;
    int cols_ = 0, rows_ = 0, margin_ = 1, textW_ = 0, x0_ = 0, y0_ = 0;
    uint32_t rowHash_[kMaxRows];
    std::string statusCache_;
    bool statusOverlay_ = false;

    // syntax highlighting: lexer state cached at the top of the screen
    syn::Lexer lex_;
    syn::LexState synState_;
    uint32_t synPos_ = kNone;
    uint8_t synLang_ = syn::None;


    std::string msg_;
    uint32_t msgUntil_ = 0;
    std::string lastFind_, lastReplace_;
    int battery_ = -1;
    uint32_t nextBattery_ = 0;
    bool warnedFull_ = false;
};
