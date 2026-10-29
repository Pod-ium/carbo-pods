// Pod Writer - distraction-free text editor firmware for the
// ClockworkPi PicoCalc with a Raspberry Pi Pico 2 W, built on the PicoPods libraries.
#include "app.h"
#include "diag.h"

int main() {
    pd::diag::begin("Pod Writer", FW_VERSION, diag::stageNames(), diag::kStageCount);
    diag::stage(diag::kStart);
    pd::Terminal& term = pd::terminal_init();   // UART debug output on GP0/GP1
    diag::stage(diag::kDisplay);
    pd::gfx::init(term.display());
    pd::diag::display_ready();
    diag::stage(diag::kKeyboard);
    static App app(term.keyboard());
    app.run();
    return 0;
}
