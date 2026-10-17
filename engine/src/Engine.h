// JAJA Engine — the audio core: sequencer, mixer, built-in instruments/effects,
// plugin hosting, recording and offline bounce. It has no GUI and no device code,
// so it can be unit-tested headless; Main.cpp connects it to a real audio device.
#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors_headless/juce_audio_processors_headless.h>
#include <algorithm>
#include <atomic>
#include <tuple>
#include <functional>
#include <map>
#include <memory>
#include <vector>
#include "Fx.h"
#include "Synths.h"

namespace jaja
{
struct Media
{
    juce::AudioBuffer<float> original;
    double originalRate = 44100;
    juce::AudioBuffer<float> data;   // resampled to the engine rate
    juce::String path;
};

struct PluginSlot
{
    juce::String key, pid, name;
    std::unique_ptr<juce::AudioPluginInstance> instance;
};

struct NoteEv { double on, off; int pitch; int vel; };
struct ClipEv { double start, end; std::shared_ptr<Media> media; double offsetSec; float gain; };

struct Insert
{
    juce::String id, type;
    bool on = true;
    std::unique_ptr<Fx> fx;                  // built-in effect, or
    std::shared_ptr<PluginSlot> plugin;      // a VST3 plugin
    juce::String lastParams;
};

struct Channel
{
    juce::String id;
    int role = 0;                            // 0 track, 1 aux bus, 2 master
    juce::String kind, inst;                 // track kind: synth / drums / audio
    float vol = 0.7071f, pan = 0;
    bool mute = false, solo = false;
    std::vector<std::unique_ptr<Insert>> inserts;
    std::vector<std::pair<juce::String, float>> sends;
    bool autoVol = false, autoPan = false;
    std::vector<std::pair<double, float>> volPts, panPts;
    BuiltinSynth synth;
    DrumKit drums;
    std::shared_ptr<PluginSlot> instPlugin;
    std::vector<NoteEv> notes;
    std::vector<ClipEv> clips;
    std::vector<std::pair<int, double>> active;   // sounding notes: pitch, off-beat
    std::vector<int> inputs;                      // device input channels (0-based)
    bool arm = false, monitor = false;
    juce::AudioBuffer<float> buf;
    juce::MidiBuffer midi;
    float curGain = -1, curM[4] { 1, 0, 0, 1 };
    std::atomic<float> peakL { 0 }, peakR { 0 };
};

struct Take
{
    juce::String trackId, id;
    juce::File file;
    std::vector<int> inputs;
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
    double startBeat = 0;
    juce::int64 skip = 0, written = 0;
};

class Engine : public juce::AudioPlayHead
{
public:
    using PluginFactory = std::function<std::unique_ptr<juce::AudioPluginInstance> (const juce::String& pid, double sr, int bs, juce::String& err)>;

    Engine();
    ~Engine() override;

    PluginFactory pluginFactory;                                   // set by the host (Main.cpp)
    std::function<void (const juce::String& key)> onPluginRemoved; // called on the message thread before a plugin is deleted
    std::function<void (const juce::var&)> emit;                   // events for the UI

    // device side (audio thread except prepare)
    void prepare (double sampleRate, int maxBlockSize);
    void process (const float* const* in, int numIn, float* const* out, int numOut, int numSamples);

    // control side (message thread)
    void setProject (const juce::var& project);
    bool loadMedia (const juce::String& id, const juce::File& file, juce::String& error);
    void play (double fromBeat, double metronomeUntil = -1.0e9);   // metronomeUntil: count-in clicks before this beat
    void stop();
    void locate (double beat);
    bool startRecord (const juce::Array<juce::var>& trackIds, double startBeat, bool countIn, const juce::File& folder, int latencySamples, juce::String& error);
    juce::var stopRecord();
    void liveNote (const juce::String& trackId, int pitch, int velocity);
    juce::var takeMeters();
    juce::var pluginStates();
    std::shared_ptr<PluginSlot> findPlugin (const juce::String& key);
    bool bounce (const juce::File& file, double endBeat, double tailSeconds, std::function<void (float)> progress, juce::String& error);

    double getBeat() const      { return beat.load(); }
    bool isPlaying() const      { return playing.load(); }
    bool isRecording() const    { return recording.load(); }
    double getSampleRate() const { return sr; }
    int numMedia() const        { return (int) media.size(); }

    juce::Optional<PositionInfo> getPosition() const override;

private:
    void processChunk (const float* const* in, int numIn, float* const* out, int numOut, int n);
    void sequence (Channel& c, int s0, int len, double b0, double bps);
    void releaseActive (Channel& c, int pos);
    void runPlugin (PluginSlot& p, juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi, int n, bool isInstrument);
    void runInserts (Channel& c, int n);
    void mixChannel (Channel& c, juce::AudioBuffer<float>& dest, int n, bool anySolo, double atBeat);
    void writeTakes (const float* const* in, int numIn, int n, double b0, double bps);
    void renderClicks (int s0, int len, double b0, double bps);
    void prepareChannel (Channel& c);
    void resampleMedia (Media& m) const;
    std::shared_ptr<PluginSlot> obtainPlugin (const juce::String& key, const juce::var& spec, std::map<juce::String, std::shared_ptr<PluginSlot>>& keep);
    void updateChannel (Channel& c, const juce::var& o, int role, std::map<juce::String, std::shared_ptr<PluginSlot>>& keep);

    juce::CriticalSection lock;
    juce::SpinLock liveLock;
    std::vector<std::tuple<juce::String, int, int>> liveQueue;

    double sr = 44100;
    int maxBlock = 512;
    std::vector<std::unique_ptr<Channel>> tracks, aux;
    std::unique_ptr<Channel> master;
    std::map<juce::String, std::shared_ptr<PluginSlot>> plugins;
    std::map<juce::String, std::shared_ptr<Media>> media;
    juce::var lastProject;

    std::atomic<double> beat { 0 };
    std::atomic<bool> playing { false }, recording { false }, offline { false };
    double bpm = 96, loopStart = 0, loopEnd = 16, recStart = 0, clickUntil = -1.0e9;
    bool loopOn = false, metro = false, countIn = false;
    double blockBeat = 0;

    std::vector<std::unique_ptr<Take>> takes;
    juce::TimeSliceThread writerThread { "JAJA recorder" };

    juce::AudioBuffer<float> scratch, clickBuf, silence;
    std::atomic<int> numInputs { 0 };
    int clickLeft = 0; double clickPhase = 0, clickFreq = 1320; float clickAmp = 0, clickDecay = 0;
    std::atomic<float> inPeaks[64];
    std::atomic<float> masterPeak[2];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Engine)
};
} // namespace jaja
