// Pod Tunes - the song model, the 8-bit synth, the sequencer and the JSON
// song format (the same format the "Pixel Plains" web creator exports).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace podtunes {

constexpr int kSteps = 32;      // steps per pattern (2 bars of 16th notes)
constexpr int kPatterns = 8;
constexpr int kRows = 24;       // melodic rows shown per channel (2 octaves)
constexpr int kChannels = 4;
constexpr int kMaxOrder = 32;

enum Channel { CH_LEAD = 0, CH_ARP = 1, CH_BASS = 2, CH_DRUMS = 3 };
enum Wave { WAVE_SQ25 = 0, WAVE_SQ50 = 1, WAVE_SQ12 = 2, WAVE_TRI = 3 };
enum Drum { DRUM_NONE = 0, DRUM_KICK = 1, DRUM_SNARE = 2, DRUM_HAT = 3 };

extern const char* const kChannelNames[kChannels];   // "LEAD" "ARP" "BASS" "DRM"
extern const char* const kWaveNames[4];              // "SQ25" "SQ50" "SQ12" "TRI"
extern const char* const kDrumNames[3];              // "KICK" "SNAR" "HAT"
extern const char* const kDrumJson[3];               // "kick" "snare" "hat"
extern const int kChannelBase[3];                    // grid base midi: C4 C4 C2
extern const float kChannelVol[kChannels];           // lead .16 arp .055 bass .24 drums 1

struct Note {
    uint8_t midi = 0;   // 0 = empty cell
    uint8_t len = 1;    // in steps
};

struct Pattern {
    Note mel[3][kSteps];      // lead, arp, bass
    uint8_t drums[kSteps] = {};   // Drum
    void clear();
};

struct ChanState {
    bool on = true;
    bool solo = false;
    int vol = 100;             // percent
    int wave = WAVE_SQ25;
};

struct Song {
    std::string name = "UNTITLED";
    int tempo = 152;           // BPM
    std::vector<int> order;    // pattern indices played in song mode
    Pattern patterns[kPatterns];
    void reset();              // back to an empty song, without a temporary
};

struct EditState {
    int cur_pat = 0;
    bool pattern_mode = false;   // false = play the song order
    int edit_ch = CH_LEAD;
    int octave = 4;
    int note_len = 2;
    int cursor_step = 0;
    int cursor_row = kRows - 1;
    ChanState ch[kChannels];
};

// ---------------------------------------------------------------- music theory
int note_to_midi(const std::string& n);        // "E5" -> 76; invalid -> 60
std::string midi_to_note(int m);               // 76 -> "E5"
double note_freq(int m);                       // 69 -> 440 Hz

struct SongPos {
    int pat = 0;
    int step = 0;
};
SongPos song_pos(const Song& s, bool pattern_mode, int cur_pat, long abs_step);
long step_samples(const Song& s, int sample_rate);   // one 16th note, in samples

// ---------------------------------------------------------------- model
bool ch_audible(const EditState& e, int ch);
float eff_vol(const EditState& e, int ch);     // 0..1: channel gain * volume percent
void build_demo(Song& s);                      // the "PIXEL PLAINS ZONE" theme
std::string song_to_json(const Song& s, bool pretty);
bool song_from_json(const std::string& text, Song& out);   // {"song":{...}} or {...}
std::string slug(const std::string& s);

// ---------------------------------------------------------------- synth
// Renders 16-bit stereo blocks and owns the voices (one per channel plus
// previews for the auditioned notes).
class Synth {
public:
    void start(int sample_rate);
    int sample_rate() const { return sr_; }
    uint64_t now() const { return now_smp_; }
    void note_on(int ch, uint8_t midi, int wave, uint64_t at_smp, uint64_t dur_smp, float vol);
    void drum_on(int drum, uint64_t at_smp, float vol);
    void audition(int ch, uint8_t midi, int wave);   // short preview at now()
    void audition_drum(int drum);
    void clear();
    void render(int frames, int16_t* stereo);        // advances now()
    float peak() const { return peak_; }             // last block's peak, for the viz
    const float* ch_levels() const { return ch_level_; }

private:
    struct Voice {
        bool on = false;
        int ch = 0;
        bool drum = false;
        int type = 0;          // Drum
        double phase = 0;
        double freq = 0;
        int wave = 0;
        uint64_t start = 0, dur = 1;
        float vol = 0;
        double prev_n = 0;
    };
    Voice voices_[8];          // 0..3 = channels, 4..7 = previews
    uint64_t now_smp_ = 0;
    int sr_ = 22050;
    uint32_t rng_ = 0x2545F491u;
    float peak_ = 0;
    float ch_level_[4] = {0, 0, 0, 0};
    float voice_sample(Voice& v, uint64_t age_smp);
};

// ---------------------------------------------------------------- sequencer
// Schedules one song step (all four channels) into the synth at sample time at_smp.
void schedule_step(const Song& s, const EditState& e, Synth& synth, uint64_t at_smp, long abs_step);

class Player {
public:
    void attach(const Song* s, const EditState* e, Synth* synth);
    void play(long from_step);
    void stop();
    bool playing() const { return playing_; }
    long step_smp() const;
    void schedule_ahead(uint64_t horizon_smp);   // steps starting before now()+horizon
    SongPos pos() const;                          // playhead (while playing)

private:
    const Song* song_ = nullptr;
    const EditState* edit_ = nullptr;
    Synth* synth_ = nullptr;
    bool playing_ = false;
    long sched_step_ = 0;
    uint64_t anchor_ = 0;                         // sample time of abs step 0
};

// ---------------------------------------------------------------- WAV export
// Renders the song (or 4 loops of the pattern) to a 16-bit stereo .wav file,
// streamed so it never sits in RAM. progress(done, total) returns false to
// cancel (the partial file is removed).
bool export_wav(const Song& s, const EditState& e, const std::string& path,
                const std::function<bool(long done, long total)>& progress);

}  // namespace podtunes
