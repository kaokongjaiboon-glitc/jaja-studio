#include "Engine.h"

namespace jaja
{
namespace
{
    float faderGain (float p) { return p * p * 2.0f; }

    float autoValue (const std::vector<std::pair<double, float>>& pts, double b)
    {
        if (pts.empty()) return 0;
        if (b <= pts.front().first) return pts.front().second;
        for (size_t i = 1; i < pts.size(); ++i)
            if (b <= pts[i].first)
            {
                const auto& a = pts[i - 1]; const auto& c = pts[i];
                return a.second + (c.second - a.second) * (float) ((b - a.first) / juce::jmax (1e-9, c.first - a.first));
            }
        return pts.back().second;
    }

    void panMatrix (float pan, float m[4])
    {
        pan = juce::jlimit (-1.0f, 1.0f, pan);
        const float x = pan <= 0 ? pan + 1 : pan;
        const float gl = std::cos (x * juce::MathConstants<float>::halfPi), gr = std::sin (x * juce::MathConstants<float>::halfPi);
        if (pan <= 0) { m[0] = 1; m[1] = gl; m[2] = 0; m[3] = gr; }
        else          { m[0] = gl; m[1] = 0; m[2] = gr; m[3] = 1; }
    }

    float softClip (float x)
    {
        x = juce::jlimit (-1.0f, 1.0f, x);
        const float a = std::abs (x);
        return a < 0.9f ? x : std::copysign (0.9f + 0.1f * std::tanh ((a - 0.9f) / 0.1f), x);
    }

    void updatePeak (std::atomic<float>& p, float v)
    {
        float cur = p.load();
        while (v > cur && ! p.compare_exchange_weak (cur, v)) {}
    }

    std::vector<std::pair<double, float>> points (const juce::var& arr)
    {
        std::vector<std::pair<double, float>> out;
        if (auto* a = arr.getArray())
            for (auto& p : *a) out.emplace_back ((double) p["b"], (float) (double) p["v"]);
        std::sort (out.begin(), out.end(), [] (auto& x, auto& y) { return x.first < y.first; });
        return out;
    }

    juce::String pluginKey (const juce::String& owner, const juce::String& slot) { return owner + ":" + slot; }
}

Engine::Engine()
{
    for (auto& p : inPeaks) p = 0;
    for (auto& p : masterPeak) p = 0;
    writerThread.startThread();
    master = std::make_unique<Channel>();
    master->id = "master"; master->role = 2;
    prepare (44100, 512);
}

Engine::~Engine()
{
    {
        const juce::ScopedLock sl (lock);
        playing = false; recording = false;
        takes.clear();
    }
    writerThread.stopThread (2000);
    tracks.clear(); aux.clear(); master.reset();
    plugins.clear();
}

//==============================================================================
void Engine::prepareChannel (Channel& c)
{
    c.buf.setSize (2, maxBlock, false, true, true);
    c.midi.ensureSize (4096);
    c.synth.prepare (sr); c.synth.setInstrument (c.inst.isEmpty() ? "keys" : c.inst);
    c.drums.prepare (sr);
    for (auto& ins : c.inserts)
        if (ins->fx) { ins->fx->prepare (sr, maxBlock); ins->lastParams.clear(); }
    c.curGain = -1;
}

void Engine::resampleMedia (Media& m) const
{
    const int ch = m.original.getNumChannels(), len = m.original.getNumSamples();
    if (std::abs (m.originalRate - sr) < 0.5)
    {
        m.data.makeCopyOf (m.original);
        return;
    }
    const double ratio = m.originalRate / sr;
    const int outLen = (int) std::ceil (len / ratio);
    m.data.setSize (ch, outLen);
    for (int c = 0; c < ch; ++c)
    {
        juce::LagrangeInterpolator li;
        li.process (ratio, m.original.getReadPointer (c), m.data.getWritePointer (c), outLen, len, 0);
    }
}

void Engine::prepare (double sampleRate, int maxBlockSize)
{
    const juce::ScopedLock sl (lock);
    sr = sampleRate > 0 ? sampleRate : 44100;
    maxBlock = juce::jmax (32, maxBlockSize);
    scratch.setSize (64, maxBlock);
    clickBuf.setSize (1, maxBlock);
    silence.setSize (1, maxBlock); silence.clear();
    for (auto& t : tracks) prepareChannel (*t);
    for (auto& a : aux) prepareChannel (*a);
    prepareChannel (*master);
    for (auto& [k, p] : plugins)
        if (p->instance) { p->instance->releaseResources(); p->instance->setRateAndBufferSizeDetails (sr, maxBlock); p->instance->prepareToPlay (sr, maxBlock); }
    for (auto& [id, m] : media) resampleMedia (*m);
    // parameters (delay time etc.) depend on the sample rate: re-apply the project
    if (! lastProject.isVoid())
    {
        const juce::ScopedUnlock ul (lock);
        setProject (lastProject);
    }
}

//==============================================================================
std::shared_ptr<PluginSlot> Engine::obtainPlugin (const juce::String& key, const juce::var& spec, std::map<juce::String, std::shared_ptr<PluginSlot>>& keep)
{
    const juce::String pid = spec["pid"].toString();
    if (pid.isEmpty()) return nullptr;
    if (auto it = keep.find (key); it != keep.end()) return it->second;
    if (auto it = plugins.find (key); it != plugins.end() && it->second->pid == pid)
    {
        keep[key] = it->second;
        return it->second;
    }
    if (! pluginFactory) return nullptr;
    juce::String err;
    auto inst = pluginFactory (pid, sr, maxBlock, err);
    if (inst == nullptr)
    {
        if (emit)
        {
            juce::DynamicObject::Ptr o = new juce::DynamicObject();
            o->setProperty ("ev", "pluginError"); o->setProperty ("key", key); o->setProperty ("pid", pid); o->setProperty ("message", err);
            emit (juce::var (o.get()));
        }
        return nullptr;
    }
    inst->setPlayHead (this);
    inst->setRateAndBufferSizeDetails (sr, maxBlock);
    inst->prepareToPlay (sr, maxBlock);
    const juce::String state = spec["state"].toString();
    if (state.isNotEmpty())
    {
        juce::MemoryBlock mb;
        if (mb.fromBase64Encoding (state)) inst->setStateInformation (mb.getData(), (int) mb.getSize());
    }
    auto slot = std::make_shared<PluginSlot>();
    slot->key = key; slot->pid = pid; slot->name = inst->getName();
    slot->instance = std::move (inst);
    keep[key] = slot;
    return slot;
}

void Engine::updateChannel (Channel& c, const juce::var& o, int role, std::map<juce::String, std::shared_ptr<PluginSlot>>& keep)
{
    c.role = role;
    c.vol = (float) (double) o.getProperty ("vol", 0.7071);
    c.pan = (float) (double) o.getProperty ("pan", 0.0);
    c.mute = (bool) o.getProperty ("mute", false);
    c.solo = (bool) o.getProperty ("solo", false);

    // inserts: keep running effects (and their tails) when only parameters change
    std::vector<std::unique_ptr<Insert>> next;
    if (auto* fx = o["fx"].getArray())
        for (auto& f : *fx)
        {
            const juce::String id = f["id"].toString(), type = f["type"].toString();
            std::unique_ptr<Insert> ins;
            for (auto& old : c.inserts)
                if (old && old->id == id && old->type == type) { ins = std::move (old); break; }
            if (! ins)
            {
                ins = std::make_unique<Insert>();
                ins->id = id; ins->type = type;
                if (type != "vst")
                {
                    ins->fx = makeBuiltinFx (type);
                    if (ins->fx) ins->fx->prepare (sr, maxBlock);
                }
            }
            ins->on = (bool) f.getProperty ("on", true);
            if (type == "vst") ins->plugin = keep.count (pluginKey (c.id, id)) ? keep[pluginKey (c.id, id)] : nullptr;
            else if (ins->fx)
            {
                const auto params = juce::JSON::toString (f["p"], true) + "@" + juce::String (bpm);
                if (params != ins->lastParams) { ins->fx->setParams (f["p"], bpm); ins->lastParams = params; }
            }
            next.push_back (std::move (ins));
        }
    c.inserts = std::move (next);
    if (role != 0) return;

    c.sends.clear();
    if (auto* so = o["sends"].getDynamicObject())
        for (auto& p : so->getProperties()) c.sends.emplace_back (p.name.toString(), (float) (double) p.value);

    const auto& au = o["auto"];
    c.autoVol = (bool) au["on"]["vol"]; c.autoPan = (bool) au["on"]["pan"];
    c.volPts = points (au["vol"]); c.panPts = points (au["pan"]);

    c.kind = o["kind"].toString();
    const juce::String inst = o["inst"].toString();
    if (inst != c.inst) { c.inst = inst; c.synth.setInstrument (inst.isEmpty() ? "keys" : inst); }
    c.instPlugin = keep.count (pluginKey (c.id, "inst")) ? keep[pluginKey (c.id, "inst")] : nullptr;

    c.inputs.clear();
    if (auto* in = o["input"].getArray()) for (auto& x : *in) c.inputs.push_back ((int) x);
    if (c.inputs.empty()) c.inputs.push_back (0);
    c.arm = (bool) o.getProperty ("arm", false);
    c.monitor = (bool) o.getProperty ("monitor", false);

    // flatten regions (with loops) into note and clip lists
    c.notes.clear(); c.clips.clear();
    if (auto* regions = o["regions"].getArray())
        for (auto& r : *regions)
        {
            const double start = (double) r["start"], len = (double) r["len"];
            const int loops = juce::jmax (1, (int) r.getProperty ("loops", 1));
            if (len <= 0) continue;
            for (int k = 0; k < loops; ++k)
            {
                const double base = start + k * len;
                if (c.kind == "audio")
                {
                    auto it = media.find (r["audioId"].toString());
                    if (it != media.end())
                        c.clips.push_back ({ base, base + len, it->second, (double) r.getProperty ("offset", 0.0), (float) (double) r.getProperty ("gain", 1.0),
                                             (double) r.getProperty ("fadeIn", 0.0), (double) r.getProperty ("fadeOut", 0.0) });
                    continue;
                }
                if (auto* notes = r["notes"].getArray())
                    for (auto& n : *notes)
                    {
                        const double s = (double) n["s"], l = (double) n["l"];
                        if (s < 0 || s >= len) continue;
                        c.notes.push_back ({ base + s, base + s + juce::jmax (0.01, juce::jmin (l, len - s)), (int) n["p"], juce::jlimit (1, 127, (int) n.getProperty ("v", 100)) });
                    }
            }
        }
    std::sort (c.notes.begin(), c.notes.end(), [] (const NoteEv& a, const NoteEv& b) { return a.on < b.on; });
}

void Engine::setProject (const juce::var& project)
{
    lastProject = project;
    // 1. plugins (slow: created outside the audio lock)
    std::map<juce::String, std::shared_ptr<PluginSlot>> keep;
    auto scanFx = [&] (const juce::String& owner, const juce::var& fxList)
    {
        if (auto* fx = fxList.getArray())
            for (auto& f : *fx)
                if (f["type"].toString() == "vst") obtainPlugin (pluginKey (owner, f["id"].toString()), f["vst"], keep);
    };
    if (auto* ts = project["tracks"].getArray())
        for (auto& t : *ts)
        {
            if (t["vst"].isObject() && t["kind"].toString() == "synth") obtainPlugin (pluginKey (t["id"].toString(), "inst"), t["vst"], keep);
            scanFx (t["id"].toString(), t["fx"]);
        }
    if (auto* as = project["aux"].getArray())
        for (auto& a : *as) scanFx (a["id"].toString(), a["fx"]);
    scanFx ("master", project["master"]["fx"]);

    // 2. swap the new structure in
    std::vector<std::unique_ptr<Channel>> graveyard;
    std::map<juce::String, std::shared_ptr<PluginSlot>> oldPlugins;
    {
        const juce::ScopedLock sl (lock);
        bpm = juce::jlimit (20.0, 400.0, (double) project.getProperty ("bpm", 96));
        tempoMap.assign (1, { 0.0, bpm });
        if (auto* tl = project["tempos"].getArray())
        {
            std::vector<std::pair<double, double>> ch;
            for (auto& t : *tl) if ((double) t["b"] > 1e-9) ch.emplace_back ((double) t["b"], juce::jlimit (20.0, 400.0, (double) t["bpm"]));
            std::sort (ch.begin(), ch.end());
            for (auto& c : ch) tempoMap.push_back (c);
        }
        {
            // signatures arrive as (bar index, num, den); store as (start beat, bar length in quarter beats)
            std::vector<std::tuple<int, int, int>> sg;
            if (auto* sl = project["sigs"].getArray())
                for (auto& x : *sl) sg.emplace_back ((int) x["bar"], juce::jlimit (1, 32, (int) x["num"]), juce::jlimit (1, 32, (int) x["den"]));
            std::sort (sg.begin(), sg.end());
            if (sg.empty() || std::get<0> (sg.front()) != 0) sg.insert (sg.begin(), { 0, 4, 4 });
            barMap.clear();
            double startBeat = 0; int prevBar = 0; double prevLen = 4;
            for (auto& [bar, num, den] : sg)
            {
                if (bar < prevBar) continue;
                startBeat += (bar - prevBar) * prevLen;
                prevLen = num * 4.0 / den; prevBar = bar;
                if (! barMap.empty() && std::abs (barMap.back().first - startBeat) < 1e-9) barMap.back().second = prevLen;
                else barMap.emplace_back (startBeat, prevLen);
            }
        }
        loopOn = (bool) project["loop"]["on"];
        loopStart = (double) project["loop"]["start"]; loopEnd = (double) project["loop"]["end"];
        metro = (bool) project.getProperty ("metro", false);

        auto sync = [&] (std::vector<std::unique_ptr<Channel>>& list, const juce::var& arr, int role)
        {
            std::vector<std::unique_ptr<Channel>> next;
            if (auto* a = arr.getArray())
                for (auto& o : *a)
                {
                    const juce::String id = o["id"].toString();
                    std::unique_ptr<Channel> ch;
                    for (auto& old : list) if (old && old->id == id) { ch = std::move (old); break; }
                    if (! ch)
                    {
                        ch = std::make_unique<Channel>();
                        ch->id = id; ch->inst = o["inst"].toString();
                        prepareChannel (*ch);
                    }
                    updateChannel (*ch, o, role, keep);
                    next.push_back (std::move (ch));
                }
            for (auto& old : list) if (old) graveyard.push_back (std::move (old));
            list = std::move (next);
        };
        sync (aux, project["aux"], 1);
        sync (tracks, project["tracks"], 0);
        updateChannel (*master, project["master"], 2, keep);
        for (auto& [k, p] : plugins) if (! keep.count (k)) oldPlugins[k] = p;
        maxLatency = 0;
        for (auto& t : tracks)
        {
            int lat = t->instPlugin && t->instPlugin->instance ? t->instPlugin->instance->getLatencySamples() : 0;
            for (auto& i : t->inserts) if (i->on && i->plugin && i->plugin->instance) lat += i->plugin->instance->getLatencySamples();
            t->latency = juce::jlimit (0, (int) sr, lat);
            maxLatency = juce::jmax (maxLatency, t->latency);
        }
        const size_t need = (size_t) juce::nextPowerOfTwo (maxLatency + maxBlock + 1);
        for (auto& t : tracks)
            if (maxLatency > t->latency && t->pdc[0].size() < need)
                for (auto& d : t->pdc) d.assign (need, 0.0f);
        plugins = keep;
    }
    // 3. tidy up outside the lock
    graveyard.clear();
    for (auto& [k, p] : oldPlugins)
        if (onPluginRemoved) onPluginRemoved (k);
    oldPlugins.clear();
}

bool Engine::loadMedia (const juce::String& id, const juce::File& file, juce::String& error)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
    if (reader == nullptr) { error = "cannot read " + file.getFullPathName(); return false; }
    auto m = std::make_shared<Media>();
    m->path = file.getFullPathName();
    m->originalRate = reader->sampleRate;
    const int ch = juce::jlimit (1, 2, (int) reader->numChannels);
    m->original.setSize (ch, (int) reader->lengthInSamples);
    reader->read (&m->original, 0, (int) reader->lengthInSamples, 0, true, ch > 1);
    {
        const juce::ScopedLock sl (lock);
        resampleMedia (*m);
        media[id] = m;
    }
    if (! lastProject.isVoid()) setProject (lastProject);   // clips that were waiting for this file
    return true;
}

//==============================================================================
double Engine::bpmAt (double b) const
{
    double v = tempoMap.front().second;
    for (auto& [tb, tv] : tempoMap) { if (tb <= b + 1e-9) v = tv; else break; }
    return v;
}

double Engine::secAt (double b) const
{
    if (b <= 0) return b * 60.0 / tempoMap.front().second;
    double s = 0, pb = 0, pv = tempoMap.front().second;
    for (size_t i = 1; i < tempoMap.size(); ++i)
    {
        if (tempoMap[i].first >= b) break;
        s += (tempoMap[i].first - pb) * 60.0 / pv;
        pb = tempoMap[i].first; pv = tempoMap[i].second;
    }
    return s + (b - pb) * 60.0 / pv;
}

double Engine::nextTempoChange (double b) const
{
    for (auto& [tb, tv] : tempoMap) if (tb > b + 1e-9) return tb;
    return 1.0e12;
}

bool Engine::isBarStart (double b) const
{
    if (b < 0) return std::abs (std::fmod (b, 4.0)) < 1e-6;
    size_t i = 0;
    while (i + 1 < barMap.size() && barMap[i + 1].first <= b + 1e-9) ++i;
    const double rel = (b - barMap[i].first) / barMap[i].second;
    return std::abs (rel - std::round (rel)) * barMap[i].second < 1e-6;
}

void Engine::applyPdc (Channel& c, int n)
{
    const int d = maxLatency - c.latency;
    if (d <= 0 || c.pdc[0].empty()) return;
    const int size = (int) c.pdc[0].size(), mask = size - 1;
    if (d >= size) return;
    for (int ch = 0; ch < 2; ++ch)
    {
        auto* x = c.buf.getWritePointer (ch);
        auto& line = c.pdc[ch];
        int w = c.pdcPos;
        for (int i = 0; i < n; ++i, w = (w + 1) & mask)
        {
            line[(size_t) w] = x[i];
            x[i] = line[(size_t) ((w - d) & mask)];
        }
    }
    c.pdcPos = (c.pdcPos + n) & mask;
}

void Engine::play (double fromBeat, double metronomeUntil)
{
    const juce::ScopedLock sl (lock);
    clickUntil = metronomeUntil;
    for (auto& t : tracks) releaseActive (*t, 0);
    beat = fromBeat; blockBeat = fromBeat;
    playing = true;
}

void Engine::stop()
{
    const juce::ScopedLock sl (lock);
    playing = false;
    for (auto& t : tracks) releaseActive (*t, 0);
}

void Engine::locate (double b)
{
    const juce::ScopedLock sl (lock);
    for (auto& t : tracks) releaseActive (*t, 0);
    beat = b; blockBeat = b;
}

void Engine::liveNote (const juce::String& trackId, int pitch, int velocity)
{
    const juce::SpinLock::ScopedLockType sl (liveLock);
    if (liveQueue.size() < 512) liveQueue.emplace_back (trackId, pitch, velocity);
}

bool Engine::startRecord (const juce::Array<juce::var>& trackIds, double startBeat, bool ci, const juce::File& folder, int latencySamples, juce::String& error)
{
    folder.createDirectory();
    std::vector<std::unique_ptr<Take>> next;
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S");
    for (auto& idv : trackIds)
    {
        const juce::String id = idv.toString();
        std::vector<int> inputs { 0 };
        {
            const juce::ScopedLock sl (lock);
            for (auto& t : tracks) if (t->id == id) inputs = t->inputs;
        }
        auto take = std::make_unique<Take>();
        take->trackId = id;
        take->id = "rec" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()).substring (0, 10);
        take->file = folder.getChildFile ("Take " + stamp + " " + take->id + ".wav");
        take->inputs = inputs;
        take->startBeat = startBeat;
        take->skip = juce::jmax (0, latencySamples) + maxLatency;
        std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream> (take->file);
        if (! static_cast<juce::FileOutputStream*> (os.get())->openedOk()) { error = "cannot write " + take->file.getFullPathName(); return false; }
        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels ((int) juce::jmin ((size_t) 2, inputs.size())).withBitsPerSample (24));
        if (writer == nullptr) { error = "cannot create WAV writer"; return false; }
        take->writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (writer.release(), writerThread, 1 << 17);
        next.push_back (std::move (take));
    }
    if (next.empty()) { error = "no track to record"; return false; }
    const juce::ScopedLock sl (lock);
    takes = std::move (next);
    recStart = startBeat; countIn = ci; clickUntil = ci ? startBeat : -1.0e9;
    for (auto& t : tracks) releaseActive (*t, 0);
    beat = startBeat - (ci ? 4.0 : 0.0); blockBeat = beat;
    recording = true; playing = true;
    return true;
}

juce::var Engine::stopRecord()
{
    std::vector<std::unique_ptr<Take>> done;
    {
        const juce::ScopedLock sl (lock);
        recording = false; playing = false;
        for (auto& t : tracks) releaseActive (*t, 0);
        done = std::move (takes);
        takes.clear();
    }
    juce::Array<juce::var> out;
    for (auto& t : done)
    {
        t->writer.reset();   // flushes the file
        if (t->written <= 0) { t->file.deleteFile(); continue; }
        juce::String err;
        loadMedia (t->id, t->file, err);
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("track", t->trackId); o->setProperty ("id", t->id);
        o->setProperty ("path", t->file.getFullPathName());
        o->setProperty ("startBeat", t->startBeat);
        o->setProperty ("seconds", (double) t->written / sr);
        out.add (juce::var (o.get()));
    }
    return out;
}

//==============================================================================
void Engine::releaseActive (Channel& c, int pos)
{
    for (auto& a : c.active) c.midi.addEvent (juce::MidiMessage::noteOff (1, a.first), pos);
    c.active.clear();
}

void Engine::sequence (Channel& c, int s0, int len, double b0, double bps)
{
    const double b1 = b0 + len * bps;
    auto at = [&] (double b) { return s0 + juce::jlimit (0, len - 1, (int) ((b - b0) / bps)); };

    if (c.kind == "audio")
    {
        auto* L = c.buf.getWritePointer (0); auto* R = c.buf.getWritePointer (1);
        for (auto& cl : c.clips)
        {
            if (cl.end <= b0 || cl.start >= b1 || cl.media == nullptr) continue;
            const auto& d = cl.media->data;
            const int dn = d.getNumSamples(); if (dn == 0) continue;
            const double from = juce::jmax (b0, cl.start), to = juce::jmin (b1, cl.end);
            const int i0 = s0 + juce::jlimit (0, len, (int) std::llround ((from - b0) / bps));
            const int i1 = s0 + juce::jlimit (0, len, (int) std::llround ((to - b0) / bps));
            juce::int64 src = (juce::int64) std::llround ((cl.offsetSec + secAt (from) - secAt (cl.start)) * sr);
            const float* dl = d.getReadPointer (0); const float* dr = d.getReadPointer (juce::jmin (1, d.getNumChannels() - 1));
            // fade envelope (at least 3 ms at every edge so cuts never click)
            const double D = secAt (cl.end) - secAt (cl.start);
            const double fi = juce::jmin (D / 2, juce::jmax (0.003, cl.fadeIn)), fo = juce::jmin (D / 2, juce::jmax (0.003, cl.fadeOut));
            const juce::int64 base = (juce::int64) std::llround (cl.offsetSec * sr);
            for (int i = i0; i < i1; ++i, ++src)
                if (src >= 0 && src < dn)
                {
                    const double t = (double) (src - base) / sr;
                    const float g = cl.gain * (float) (juce::jmin (1.0, juce::jmax (0.0, t) / fi) * juce::jmin (1.0, juce::jmax (0.0, D - t) / fo));
                    L[i] += dl[src] * g; R[i] += dr[src] * g;
                }
        }
        return;
    }

    for (auto it = c.active.begin(); it != c.active.end();)
    {
        if (it->second < b1) { c.midi.addEvent (juce::MidiMessage::noteOff (1, it->first), at (it->second)); it = c.active.erase (it); }
        else ++it;
    }
    auto it = std::lower_bound (c.notes.begin(), c.notes.end(), b0, [] (const NoteEv& n, double b) { return n.on < b; });
    for (; it != c.notes.end() && it->on < b1; ++it)
    {
        const int pos = at (it->on);
        for (auto a = c.active.begin(); a != c.active.end();)
        {
            if (a->first == it->pitch) { c.midi.addEvent (juce::MidiMessage::noteOff (1, a->first), pos); a = c.active.erase (a); }
            else ++a;
        }
        c.midi.addEvent (juce::MidiMessage::noteOn (1, it->pitch, (juce::uint8) it->vel), pos);
        if (it->off < b1) c.midi.addEvent (juce::MidiMessage::noteOff (1, it->pitch), juce::jmax (pos, at (it->off)));
        else c.active.emplace_back (it->pitch, it->off);
    }
}

void Engine::renderClicks (int s0, int len, double b0, double bps)
{
    const double b1 = b0 + len * bps;
    for (double b = std::ceil (b0 - 1e-9); b < b1; b += 1.0)
    {
        if (! (metro || b < clickUntil - 1e-9)) continue;
        const int pos = s0 + juce::jlimit (0, len - 1, (int) ((b - b0) / bps));
        const int bi = (int) std::llround (b);
        clickFreq = isBarStart ((double) bi) ? 1760.0 : 1320.0;
        clickPhase = 0; clickAmp = 0.25f; clickLeft = (int) (0.05 * sr);
        clickDecay = std::pow (0.0001f / 0.25f, 1.0f / (float) (0.04 * sr));
        auto* d = clickBuf.getWritePointer (0);
        // render this click from pos to the end of the block; the rest continues next block
        for (int i = pos; i < clickBuf.getNumSamples() && clickLeft > 0; ++i, --clickLeft)
        {
            d[i] += (float) std::sin (clickPhase) * clickAmp;
            clickPhase += juce::MathConstants<double>::twoPi * clickFreq / sr; clickAmp *= clickDecay;
        }
    }
}

void Engine::runPlugin (PluginSlot& p, juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi, int n, bool isInstrument)
{
    auto& inst = *p.instance;
    const juce::ScopedLock pl (inst.getCallbackLock());
    if (inst.isSuspended()) { if (isInstrument) buf.clear (0, n); return; }
    const int ins = inst.getTotalNumInputChannels(), outs = inst.getTotalNumOutputChannels();
    const int chans = juce::jlimit (1, scratch.getNumChannels(), juce::jmax (ins, outs, 2));
    for (int c = 0; c < chans; ++c) scratch.clear (c, 0, n);
    if (! isInstrument)
    {
        if (ins == 1)
        {
            scratch.copyFrom (0, 0, buf, 0, 0, n); scratch.addFrom (0, 0, buf, 1, 0, n); scratch.applyGain (0, 0, n, 0.5f);
        }
        else if (ins >= 2) { scratch.copyFrom (0, 0, buf, 0, 0, n); scratch.copyFrom (1, 0, buf, 1, 0, n); }
    }
    juce::AudioBuffer<float> view (scratch.getArrayOfWritePointers(), chans, n);
    inst.processBlock (view, midi);
    if (outs <= 0) { if (isInstrument) buf.clear (0, n); return; }
    buf.copyFrom (0, 0, view, 0, 0, n);
    buf.copyFrom (1, 0, view, outs >= 2 ? 1 : 0, 0, n);
}

void Engine::runInserts (Channel& c, int n)
{
    juce::MidiBuffer none;
    for (auto& ins : c.inserts)
    {
        if (! ins->on) continue;
        if (ins->fx) ins->fx->process (c.buf.getWritePointer (0), c.buf.getWritePointer (1), n);
        else if (ins->plugin && ins->plugin->instance) { none.clear(); runPlugin (*ins->plugin, c.buf, none, n, false); }
    }
}

void Engine::mixChannel (Channel& c, juce::AudioBuffer<float>& dest, int n, bool anySolo, double atBeat)
{
    const bool gate = ! c.mute && ! (c.role == 0 && (soloOverride.isNotEmpty() ? c.id != soloOverride : (anySolo && ! c.solo)));
    const bool pl = playing.load();
    const float v = (pl && c.autoVol && ! c.volPts.empty()) ? autoValue (c.volPts, atBeat) : c.vol;
    const float pan = (pl && c.autoPan && ! c.panPts.empty()) ? autoValue (c.panPts, atBeat) : c.pan;
    const float g = gate ? faderGain (v) : 0.0f;
    float m[4]; panMatrix (pan, m);
    if (c.curGain < 0) { c.curGain = g; std::copy (m, m + 4, c.curM); }
    auto* L = c.buf.getWritePointer (0); auto* R = c.buf.getWritePointer (1);
    float pkL = 0, pkR = 0;
    const float inv = 1.0f / (float) n;
    for (int i = 0; i < n; ++i)
    {
        const float t = (float) (i + 1) * inv;
        const float gg = c.curGain + (g - c.curGain) * t;
        const float a = c.curM[0] + (m[0] - c.curM[0]) * t, b = c.curM[1] + (m[1] - c.curM[1]) * t;
        const float cc = c.curM[2] + (m[2] - c.curM[2]) * t, d = c.curM[3] + (m[3] - c.curM[3]) * t;
        const float l = L[i], r = R[i];
        L[i] = gg * (a * l + b * r); R[i] = gg * (cc * l + d * r);
        pkL = juce::jmax (pkL, std::abs (L[i])); pkR = juce::jmax (pkR, std::abs (R[i]));
    }
    c.curGain = g; std::copy (m, m + 4, c.curM);
    updatePeak (c.peakL, pkL); updatePeak (c.peakR, pkR);
    dest.addFrom (0, 0, c.buf, 0, 0, n); dest.addFrom (1, 0, c.buf, 1, 0, n);
}

void Engine::writeTakes (const float* const* in, int numIn, int n, double b0, double bps)
{
    int from = 0;
    if (b0 < recStart)
    {
        const double k = std::ceil ((recStart - b0) / bps - 1e-9);
        if (k >= n) return;
        from = (int) k;
    }
    for (auto& t : takes)
    {
        int s = from;
        if (t->skip > 0) { const int k = (int) juce::jmin ((juce::int64) (n - s), t->skip); t->skip -= k; s += k; }
        if (s >= n || ! t->writer) continue;
        const float* ptrs[2];
        const int nc = (int) juce::jmin ((size_t) 2, t->inputs.size());
        for (int c = 0; c < nc; ++c)
        {
            const int ch = t->inputs[(size_t) c];
            ptrs[c] = (ch >= 0 && ch < numIn && in[ch] != nullptr) ? in[ch] + s : silence.getReadPointer (0) + s;
        }
        if (t->writer->write (ptrs, n - s)) t->written += n - s;
    }
}

//==============================================================================
void Engine::process (const float* const* in, int numIn, float* const* out, int numOut, int numSamples)
{
    for (int c = 0; c < numOut; ++c) if (out[c]) juce::FloatVectorOperations::clear (out[c], numSamples);
    numInputs = juce::jmin (numIn, 64);
    for (int c = 0; c < juce::jmin (numIn, 64); ++c)
        if (in[c]) { const auto r = juce::FloatVectorOperations::findMinAndMax (in[c], numSamples); updatePeak (inPeaks[c], juce::jmax (r.getEnd(), -r.getStart())); }
    if (offline.load()) return;
    const juce::ScopedLock sl (lock);
    const float* ins[64]; float* outs[8];
    int done = 0;
    while (done < numSamples)
    {
        const int n = juce::jmin (maxBlock, numSamples - done);
        for (int c = 0; c < juce::jmin (numIn, 64); ++c) ins[c] = in[c] ? in[c] + done : nullptr;
        for (int c = 0; c < juce::jmin (numOut, 8); ++c) outs[c] = out[c] ? out[c] + done : nullptr;
        processChunk (ins, juce::jmin (numIn, 64), outs, juce::jmin (numOut, 8), n);
        done += n;
    }
}

void Engine::processChunk (const float* const* in, int numIn, float* const* out, int numOut, int n)
{
    for (auto& t : tracks) { t->buf.clear (0, n); }
    for (auto& a : aux) a->buf.clear (0, n);
    master->buf.clear (0, n);
    clickBuf.clear (0, n);
    // clicks carried over from the previous block
    if (clickLeft > 0)
    {
        auto* d = clickBuf.getWritePointer (0);
        for (int i = 0; i < n && clickLeft > 0; ++i, --clickLeft)
        {
            d[i] += (float) std::sin (clickPhase) * clickAmp;
            clickPhase += juce::MathConstants<double>::twoPi * clickFreq / sr; clickAmp *= clickDecay;
        }
    }

    // live notes from the UI (keyboard, MIDI keyboard, piano roll audition)
    {
        const juce::SpinLock::ScopedTryLockType tl (liveLock);
        if (tl.isLocked())
        {
            for (auto& [id, p, v] : liveQueue)
                for (auto& t : tracks)
                    if (t->id == id)
                        t->midi.addEvent (v > 0 ? juce::MidiMessage::noteOn (1, p, (juce::uint8) juce::jlimit (1, 127, v)) : juce::MidiMessage::noteOff (1, p), 0);
            liveQueue.clear();
        }
    }

    blockBeat = beat.load();
    const double bps = bpmAt (blockBeat) / 60.0 / sr;
    if (playing.load())
    {
        int s = 0; double b = blockBeat;
        while (s < n)
        {
            const double bps = bpmAt (b) / 60.0 / sr;
            int len = n - s; bool wrap = false;
            const double nt = nextTempoChange (b);
            if (nt < b + len * bps) len = juce::jlimit (1, n - s, (int) std::ceil ((nt - b) / bps - 1e-9));
            if (loopOn && ! recording.load() && loopEnd > loopStart && b < loopEnd && b + len * bps >= loopEnd)
            {
                len = juce::jlimit (1, n - s, (int) std::ceil ((loopEnd - b) / bps - 1e-9));
                wrap = true;
            }
            for (auto& t : tracks) sequence (*t, s, len, b, bps);
            renderClicks (s, len, b, bps);
            b += len * bps; s += len;
            if (wrap)
            {
                b = loopStart;
                for (auto& t : tracks) releaseActive (*t, juce::jmin (s, n - 1));
            }
        }
        if (recording.load()) writeTakes (in, numIn, n, blockBeat, bps);
        beat = b;
    }

    // instruments, input monitoring, inserts, faders
    for (auto& t : tracks)
    {
        auto& c = *t;
        if (c.kind == "audio")
        {
            if (c.monitor && c.arm)
                for (int k = 0; k < (int) c.inputs.size() && k < 2; ++k)
                {
                    const int ch = c.inputs[(size_t) k];
                    if (ch < 0 || ch >= numIn || in[ch] == nullptr) continue;
                    if (c.inputs.size() == 1) { c.buf.addFrom (0, 0, in[ch], n); c.buf.addFrom (1, 0, in[ch], n); }
                    else c.buf.addFrom (k, 0, in[ch], n);
                }
        }
        else if (c.instPlugin && c.instPlugin->instance) runPlugin (*c.instPlugin, c.buf, c.midi, n, true);
        else if (c.kind == "drums") c.drums.render (c.buf, c.midi, n);
        else c.synth.render (c.buf, c.midi, n);
        c.midi.clear();
        runInserts (c, n);
        applyPdc (c, n);
    }
    bool anySolo = false;
    for (auto& t : tracks) anySolo = anySolo || t->solo;
    for (auto& t : tracks)
    {
        mixChannel (*t, master->buf, n, anySolo, blockBeat);
        // post-fader, post-pan sends (mixChannel left the processed signal in t->buf)
        for (auto& [auxId, lvl] : t->sends)
            if (lvl > 0)
                for (auto& a : aux)
                    if (a->id == auxId) { a->buf.addFrom (0, 0, t->buf, 0, 0, n, lvl); a->buf.addFrom (1, 0, t->buf, 1, 0, n, lvl); }
    }
    for (auto& a : aux) { runInserts (*a, n); mixChannel (*a, master->buf, n, false, blockBeat); }

    auto& m = *master;
    runInserts (m, n);
    {
        const float g = faderGain (m.vol);
        if (m.curGain < 0) m.curGain = g;
        auto* L = m.buf.getWritePointer (0); auto* R = m.buf.getWritePointer (1);
        float pkL = 0, pkR = 0;
        for (int i = 0; i < n; ++i)
        {
            const float gg = m.curGain + (g - m.curGain) * (float) (i + 1) / (float) n;
            L[i] = softClip (L[i] * gg); R[i] = softClip (R[i] * gg);
            pkL = juce::jmax (pkL, std::abs (L[i])); pkR = juce::jmax (pkR, std::abs (R[i]));
        }
        m.curGain = g;
        updatePeak (m.peakL, pkL); updatePeak (m.peakR, pkR);
    }
    if (numOut >= 1 && out[0]) juce::FloatVectorOperations::copy (out[0], m.buf.getReadPointer (0), n);
    if (numOut >= 2 && out[1]) juce::FloatVectorOperations::copy (out[1], m.buf.getReadPointer (1), n);
    for (int c = 0; c < juce::jmin (2, numOut); ++c) if (out[c]) juce::FloatVectorOperations::add (out[c], clickBuf.getReadPointer (0), n);
}

//==============================================================================
juce::var Engine::takeMeters()
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject(), ch = new juce::DynamicObject();
    auto pair = [] (Channel& c)
    {
        juce::Array<juce::var> a; a.add (c.peakL.exchange (0)); a.add (c.peakR.exchange (0)); return juce::var (a);
    };
    {
        const juce::ScopedLock sl (lock);
        for (auto& t : tracks) ch->setProperty (t->id, pair (*t));
        for (auto& a : aux) ch->setProperty (a->id, pair (*a));
        ch->setProperty ("master", pair (*master));
    }
    juce::Array<juce::var> ins;
    for (int c = 0; c < numInputs.load(); ++c) ins.add (inPeaks[c].exchange (0));
    o->setProperty ("ev", "meters");
    o->setProperty ("ch", juce::var (ch.get()));
    o->setProperty ("in", ins);
    o->setProperty ("beat", beat.load());
    o->setProperty ("playing", playing.load());
    o->setProperty ("recording", recording.load());
    return juce::var (o.get());
}

juce::var Engine::pluginStates()
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    std::map<juce::String, std::shared_ptr<PluginSlot>> copy;
    { const juce::ScopedLock sl (lock); copy = plugins; }
    for (auto& [k, p] : copy)
    {
        if (! p->instance) continue;
        juce::MemoryBlock mb;
        p->instance->getStateInformation (mb);
        o->setProperty (k, mb.toBase64Encoding());
    }
    return juce::var (o.get());
}

std::shared_ptr<PluginSlot> Engine::findPlugin (const juce::String& key)
{
    const juce::ScopedLock sl (lock);
    auto it = plugins.find (key);
    return it != plugins.end() ? it->second : nullptr;
}

juce::Optional<juce::AudioPlayHead::PositionInfo> Engine::getPosition() const
{
    PositionInfo p;
    p.setBpm (bpmAt (blockBeat));
    p.setTimeSignature (juce::AudioPlayHead::TimeSignature { 4, 4 });
    p.setPpqPosition (blockBeat);
    {
        size_t i = 0;
        while (i + 1 < barMap.size() && barMap[i + 1].first <= blockBeat + 1e-9) ++i;
        const double bl = barMap[i].second, st = barMap[i].first + std::floor ((blockBeat - barMap[i].first) / bl) * bl;
        p.setPpqPositionOfLastBarStart (blockBeat < 0 ? std::floor (blockBeat / 4.0) * 4.0 : st);
        p.setTimeSignature (juce::AudioPlayHead::TimeSignature { (int) std::lround (bl), 4 });
    }
    p.setTimeInSeconds (secAt (blockBeat));
    p.setTimeInSamples ((juce::int64) (secAt (blockBeat) * sr));
    p.setIsPlaying (playing.load());
    p.setIsRecording (recording.load());
    p.setIsLooping (loopOn);
    p.setLoopPoints (juce::AudioPlayHead::LoopPoints { loopStart, loopEnd });
    return p;
}

//==============================================================================
bool Engine::bounce (const juce::File& file, double endBeat, double tailSeconds, std::function<void (float)> progress, juce::String& error, const juce::String& onlyTrack)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream> (file);
    if (! static_cast<juce::FileOutputStream*> (os.get())->openedOk()) { error = "cannot write " + file.getFullPathName(); return false; }
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (2).withBitsPerSample (24));
    if (! writer) { error = "cannot create WAV writer"; return false; }

    offline = true;
    const juce::ScopedLock sl (lock);   // the device callback now outputs silence without waiting for us
    const bool wasPlaying = playing.load(), wasLoop = loopOn, wasMetro = metro;
    const double wasBeat = beat.load();
    std::vector<std::unique_ptr<Take>> keepTakes = std::move (takes);
    const bool wasRec = recording.load();
    const double wasClick = clickUntil;
    recording = false; loopOn = false; metro = false; clickUntil = -1.0e9;
    auto resetAll = [&] (bool nonRealtime)
    {
        for (auto* list : { &tracks, &aux })
            for (auto& c : *list)
            {
                c->active.clear(); c->midi.clear(); c->synth.allNotesOff(); c->curGain = -1;
                for (auto& i : c->inserts) if (i->fx) i->fx->reset();
            }
        for (auto& i : master->inserts) if (i->fx) i->fx->reset();
        master->curGain = -1;
        for (auto& [k, p] : plugins) if (p->instance) { p->instance->setNonRealtime (nonRealtime); p->instance->reset(); }
    };
    resetAll (true);
    clickLeft = 0;
    beat = 0; blockBeat = 0; playing = true;

    soloOverride = onlyTrack;
    for (auto& t : tracks) for (auto& d : t->pdc) std::fill (d.begin(), d.end(), 0.0f);
    const juce::int64 songLen = (juce::int64) std::ceil (secAt (endBeat) * sr + maxLatency), total = songLen + (juce::int64) (tailSeconds * sr);
    juce::AudioBuffer<float> outBuf (2, maxBlock);
    juce::int64 pos = 0; int lastPct = -1;
    while (pos < total)
    {
        const int n = (int) juce::jmin ((juce::int64) maxBlock, total - pos);
        if (playing && pos + n > songLen) { playing = false; for (auto& t : tracks) releaseActive (*t, 0); }
        float* outs[2] { outBuf.getWritePointer (0), outBuf.getWritePointer (1) };
        processChunk (nullptr, 0, outs, 2, n);
        {
            // plugin latency delays everything by maxLatency samples: drop that much from the start of the file
            const int skip = (int) juce::jlimit ((juce::int64) 0, (juce::int64) n, (juce::int64) maxLatency - pos);
            if (skip < n) writer->writeFromAudioSampleBuffer (outBuf, skip, n - skip);
        }
        pos += n;
        const int pct = (int) (pos * 100 / juce::jmax ((juce::int64) 1, total));
        if (pct != lastPct && progress) { lastPct = pct; progress ((float) pct / 100.0f); }
    }
    writer.reset();

    resetAll (false);
    loopOn = wasLoop; metro = wasMetro; clickUntil = wasClick; beat = wasBeat; blockBeat = wasBeat; playing = wasPlaying && ! wasRec;
    takes = std::move (keepTakes);
    soloOverride.clear();
    offline = false;
    return true;
}
} // namespace jaja
