// Exercises the JAJA engine core: sequencer, built-in synths and FX, loop, recording, bounce
#include "Engine.h"
#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf ("FAIL: %s\n", msg); ++failures; } else std::printf ("ok:   %s\n", msg); } while (0)

static juce::String projectJson (const juce::String& extraTrack = {})
{
    juce::String notes;
    for (int i = 0; i < 16; ++i) notes << (i ? "," : "") << "{\"s\":" << i * 0.5 << ",\"l\":0.4,\"p\":" << (i % 2 ? 42 : 36) << ",\"v\":100}";
    return R"({"bpm":120,"loop":{"on":true,"start":0,"end":8},"metro":false,
      "master":{"vol":0.7071,"fx":[{"id":"m1","type":"limiter","on":true,"p":{"gain":0,"ceil":-1}}]},
      "aux":[{"id":"aux1","vol":0.7071,"pan":0,"fx":[{"id":"r1","type":"reverb","on":true,"p":{"size":2.6,"pre":15,"damp":6000,"mix":100}}]},
             {"id":"aux2","vol":0.66,"pan":0,"fx":[{"id":"d1","type":"delay","on":true,"p":{"time":0.75,"fb":38,"tone":5000,"mix":100}}]}],
      "tracks":[
        {"id":"dr","kind":"drums","vol":0.7,"pan":0,"fx":[{"id":"c1","type":"comp","on":true,"p":{"thr":-18,"ratio":3,"att":10,"rel":150,"knee":6,"makeup":2}}],"sends":{},
         "auto":{"on":{"vol":false,"pan":false},"vol":[],"pan":[]},"regions":[{"start":0,"len":8,"loops":1,"notes":[)" + notes + R"(]}]},
        {"id":"ky","kind":"synth","inst":"keys","vol":0.62,"pan":-0.3,"fx":[{"id":"e1","type":"eq","on":true,"p":{"hpf":120,"lf":120,"lg":0,"mf":1000,"mg":0,"mq":1,"hf":8000,"hg":2}},
          {"id":"ch1","type":"chorus","on":true,"p":{"rate":0.8,"depth":3,"mix":45}},{"id":"s1","type":"sat","on":true,"p":{"drive":8,"tone":9000,"mix":100,"out":-4}}],
         "sends":{"aux1":0.35,"aux2":0.15},"auto":{"on":{"vol":true,"pan":false},"vol":[{"b":0,"v":0.2},{"b":8,"v":0.8}],"pan":[]},
         "regions":[{"start":0,"len":4,"loops":2,"notes":[{"s":0,"l":3.9,"p":60,"v":90},{"s":0,"l":3.9,"p":64,"v":90},{"s":0,"l":3.9,"p":67,"v":90}]}]},
        {"id":"bs","kind":"synth","inst":"bass","vol":0.66,"pan":0,"fx":[],"sends":{},"auto":{"on":{"vol":false,"pan":false},"vol":[],"pan":[]},
         "regions":[{"start":0,"len":8,"loops":1,"notes":[{"s":0,"l":1.4,"p":36,"v":100},{"s":2,"l":1.4,"p":43,"v":100}]}]},
        {"id":"au","kind":"audio","vol":0.7071,"pan":0,"fx":[],"sends":{},"input":[1],"arm":true,"monitor":false,"auto":{"on":{"vol":false,"pan":false},"vol":[],"pan":[]},
         "regions":[{"start":4,"len":2,"loops":1,"audioId":"tone","offset":0,"gain":1}]})" + extraTrack + R"(
      ]})";
}

// a stand-in for a VST plugin that delays the signal and reports that delay as latency
struct LatencyPlugin : juce::AudioPluginInstance
{
    int d;
    std::vector<float> line[2]; int pos = 0;
    explicit LatencyPlugin (int delaySamples) : juce::AudioPluginInstance (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo()).withOutput ("Out", juce::AudioChannelSet::stereo())), d (delaySamples)
    { setLatencySamples (d); for (auto& l : line) l.assign ((size_t) d + 1, 0.0f); }
    void fillInPluginDescription (juce::PluginDescription& pd) const override { pd.name = "Latency"; }
    const juce::String getName() const override { return "Latency"; }
    void prepareToPlay (double, int) override {}
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        for (int i = 0; i < b.getNumSamples(); ++i, pos = (pos + 1) % (d + 1))
            for (int c = 0; c < 2; ++c) { const float x = b.getSample (c, i); b.setSample (c, i, line[c][(size_t) pos]); line[c][(size_t) pos] = x; }
    }
    double getTailLengthSeconds() const override { return 0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}
};

static int firstOnset (const juce::File& f, double sr)
{
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    if (! r) return -1;
    juce::AudioBuffer<float> b (2, (int) r->lengthInSamples); r->read (&b, 0, b.getNumSamples(), 0, true, true);
    for (int i = 0; i < b.getNumSamples(); ++i) if (std::abs (b.getSample (0, i)) > 0.01f) return i;
    juce::ignoreUnused (sr);
    return -1;
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    jaja::Engine e;
    const double sr = 48000; const int bs = 256;
    e.prepare (sr, bs);

    // a 1 s 440 Hz test file used as an audio clip
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("jaja-engine-test");
    dir.deleteRecursively(); dir.createDirectory();
    {
        auto f = dir.getChildFile ("tone.wav");
        std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream> (f);
        juce::WavAudioFormat wav;
        auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (44100).withNumChannels (1).withBitsPerSample (16));
        juce::AudioBuffer<float> b (1, 44100);
        for (int i = 0; i < 44100; ++i) b.setSample (0, i, 0.5f * std::sin (juce::MathConstants<float>::twoPi * 440.0f * i / 44100.0f));
        w->writeFromAudioSampleBuffer (b, 0, 44100);
    }
    auto proj = juce::JSON::parse (projectJson());
    CHECK (proj.isObject(), "project json parses");
    e.setProject (proj);
    juce::String err;
    CHECK (e.loadMedia ("tone", dir.getChildFile ("tone.wav"), err), "load media (resampled 44.1k -> 48k)");

    juce::AudioBuffer<float> in (2, bs), out (2, bs);
    in.clear();
    auto run = [&] (double seconds, float& peak, double& rms, bool feedInput = false)
    {
        peak = 0; rms = 0; juce::int64 count = 0; static double ph = 0;
        for (int blk = 0; blk < (int) (seconds * sr / bs); ++blk)
        {
            if (feedInput) for (int i = 0; i < bs; ++i) { in.setSample (1, i, 0.3f * (float) std::sin (ph)); ph += 0.05; }
            e.process (in.getArrayOfReadPointers(), 2, out.getArrayOfWritePointers(), 2, bs);
            for (int c = 0; c < 2; ++c) for (int i = 0; i < bs; ++i) { const float v = out.getSample (c, i); peak = std::max (peak, std::abs (v)); rms += v * v; ++count; }
        }
        rms = std::sqrt (rms / juce::jmax ((juce::int64) 1, count));
    };
    float pk; double rms;
    run (0.5, pk, rms);
    CHECK (pk == 0.0f, "silent while stopped");
    e.play (0);
    run (3.0, pk, rms);
    std::printf ("      play 3 s: peak %.3f rms %.4f beat %.2f\n", pk, rms, e.getBeat());
    CHECK (pk > 0.05f && pk <= 1.0f, "plays audio within 0 dBFS");
    CHECK (rms > 0.01, "mix has energy");
    CHECK (e.getBeat() >= 5.9 && e.getBeat() <= 6.1, "transport advances 6 beats in 3 s at 120 bpm");
    run (2.0, pk, rms);
    CHECK (e.getBeat() < 8.0 && e.getBeat() >= 1.9, "loop wraps back to the start");
    auto meters = e.takeMeters();
    CHECK ((float) meters["ch"]["dr"][0] > 0.0f && (float) meters["ch"]["master"][1] > 0.0f, "meters report levels");
    CHECK (meters["in"].size() == 2, "input meter count follows the device");
    e.stop();
    run (2.0, pk, rms);
    run (4.0, pk, rms);
    std::printf ("      2-6 s after stop peak %.4f\n", pk);
    CHECK (pk < 0.02f, "notes and effect tails die out after stop");

    // live note on the keys track
    e.liveNote ("ky", 72, 100);
    run (0.2, pk, rms);
    CHECK (pk > 0.001f, "live note sounds");
    e.liveNote ("ky", 72, 0);
    run (2.0, pk, rms);

    // recording input 2 onto the audio track with 256 samples of latency compensation
    CHECK (e.startRecord (juce::Array<juce::var> { "au" }, 2.0, true, dir, 256, err), "start recording with count-in");
    run (3.0, pk, rms, true);
    auto takes = e.stopRecord();
    CHECK (takes.size() == 1, "one take recorded");
    if (takes.size() == 1)
    {
        const double secs = (double) takes[0]["seconds"];
        std::printf ("      take %.3f s, %s\n", secs, takes[0]["path"].toString().toRawUTF8());
        CHECK (secs > 0.9 && secs < 1.05, "take = 3 s minus 2 s count-in minus latency");
        CHECK (juce::File (takes[0]["path"].toString()).getSize() > 1000, "take written to disk");
    }
    CHECK (e.numMedia() == 2, "take is loaded as media");

    // project update keeps playing state and swaps instruments
    auto p2 = juce::JSON::parse (projectJson().replace ("\"inst\":\"bass\"", "\"inst\":\"pluck\""));
    e.setProject (p2);
    e.play (0); run (1.0, pk, rms); e.stop();
    CHECK (pk > 0.05f, "still plays after a project update");

    // offline bounce of 8 beats + 1 s tail
    juce::Array<float> prog;
    auto mixFile = dir.getChildFile ("mix.wav");
    CHECK (e.bounce (mixFile, 8.0, 1.0, [&] (float p) { prog.add (p); }, err), "bounce");
    {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (mixFile));
        CHECK (r != nullptr && std::abs ((double) r->lengthInSamples / sr - 5.0) < 0.02, "bounce length = 4 s song + 1 s tail");
        if (r)
        {
            juce::AudioBuffer<float> b (2, (int) r->lengthInSamples); r->read (&b, 0, b.getNumSamples(), 0, true, true);
            std::printf ("      bounce peak %.3f\n", b.getMagnitude (0, b.getNumSamples()));
            CHECK (b.getMagnitude (0, b.getNumSamples()) > 0.05f && b.getMagnitude (0, b.getNumSamples()) <= 1.0f, "bounce has audio within 0 dBFS");
        }
    }
    CHECK (prog.size() > 10 && prog.getLast() == 1.0f, "bounce progress reaches 100%");
    e.play (0); run (0.5, pk, rms); e.stop();
    CHECK (pk > 0.0f, "live playback resumes after bounce");


    // ---- tempo change: 120 bpm for 4 beats, then 60 bpm -> beat 6 is at 2 s + 2 s = 4 s
    {
        auto pj = juce::JSON::parse (projectJson().replace ("\"bpm\":120,", "\"bpm\":120,\"tempos\":[{\"b\":4,\"bpm\":60}],\"sigs\":[{\"bar\":0,\"num\":3,\"den\":4}],"));
        e.setProject (pj);
        CHECK (std::abs (e.secAt (6.0) - 4.0) < 1e-9, "tempo map: beat 6 at 4.0 s");
        CHECK (e.isBarStart (3.0) && ! e.isBarStart (4.0) && e.isBarStart (6.0), "3/4 bar starts at beats 0, 3, 6");
        e.play (0); run (3.0, pk, rms); 
        std::printf ("      beat after 3 s with tempo change: %.3f\n", e.getBeat());
        CHECK (std::abs (e.getBeat() - 5.0) < 0.02, "transport follows tempo change (3 s -> beat 5)");
        e.stop(); run (0.5, pk, rms);
        auto f = dir.getChildFile ("tempo.wav");
        CHECK (e.bounce (f, 8.0, 0.0, nullptr, err), "bounce with tempo change");
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
        CHECK (r && std::abs ((double) r->lengthInSamples / sr - 6.0) < 0.02, "bounce length = 2 s + 4 s");
    }

    // ---- plugin delay compensation and stem bounce
    {
        e.pluginFactory = [] (const juce::String&, double, int, juce::String&) -> std::unique_ptr<juce::AudioPluginInstance> { return std::make_unique<LatencyPlugin> (1000); };
        const juce::String two = R"(,{"id":"au2","kind":"audio","vol":0.7071,"pan":0,"fx":[{"id":"lat","type":"vst","on":true,"vst":{"pid":"fake"}}],"sends":{},"auto":{"on":{"vol":false,"pan":false},"vol":[],"pan":[]},
           "regions":[{"start":4,"len":2,"loops":1,"audioId":"tone","offset":0,"gain":1}]})";
        auto pj = juce::JSON::parse (projectJson (two));
        e.setProject (pj);
        CHECK (e.getMaxLatency() == 1000, "latency of the VST chain is detected (1000 samples)");
        auto fa = dir.getChildFile ("stem-au.wav"), fb = dir.getChildFile ("stem-au2.wav");
        CHECK (e.bounce (fa, 8.0, 0.5, nullptr, err, "au") && e.bounce (fb, 8.0, 0.5, nullptr, err, "au2"), "stem bounce of two tracks");
        const int oa = firstOnset (fa, sr), ob = firstOnset (fb, sr);
        std::printf ("      onset plain track %d, track with 1000-sample plugin %d (expected %d)\n", oa, ob, (int) (2.0 * sr));
        CHECK (oa > 0 && std::abs (oa - ob) <= 2, "PDC lines up the delayed track with the plain one");
        CHECK (std::abs (oa - (int) (2.0 * sr)) <= 8, "stem starts exactly at beat 4 (2 s)");
        auto fd = dir.getChildFile ("stem-dr.wav");
        CHECK (e.bounce (fd, 8.0, 0.5, nullptr, err, "dr") && firstOnset (fd, sr) >= 0 && firstOnset (fd, sr) < 100, "drum stem starts at 0");
    }
    std::printf ("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
