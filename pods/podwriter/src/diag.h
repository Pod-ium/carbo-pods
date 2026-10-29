// Pod Writer - start-up stages, on top of PicoPods' pd_diag.
#pragma once
#include "pd/diag.h"

namespace diag {

enum Stage : int {
    kStart = 0,
    kDisplay,
    kKeyboard,
    kSdCard,
    kWorkDir,
    kScratch,
    kSettings,
    kLastFile,
    kFirstScreen,
    kStageCount
};

inline const char* const* stageNames() {
    static const char* const names[kStageCount] = {"start",         "display",  "keyboard",  "SD card",     "work folder",
                                                   "scratch files", "settings", "last file", "first screen"};
    return names;
}

inline void stage(Stage s) { pd::diag::stage(s); }
inline void done() { pd::diag::done(); }
inline int previousStage() { return pd::diag::previous_stage(); }

}  // namespace diag
