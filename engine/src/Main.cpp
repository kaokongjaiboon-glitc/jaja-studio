// JAJA Engine — the native audio engine for JAJA Studio.
// Runs without a main window next to the Electron UI, which talks to it with one JSON object per line
// over stdin/stdout. Provides ASIO / WASAPI devices, per-channel inputs, VST3 hosting and recording.
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "Engine.h"

#if JUCE_WINDOWS
 #include <io.h>
 #include <fcntl.h>
 static int rawRead (char* b, int n)          { return _read (0, b, (unsigned) n); }
 static int rawWrite (const char* b, int n)   { return _write (1, b, (unsigned) n); }
#else
 #include <unistd.h>
 static int rawRead (char* b, int n)          { return (int) ::read (0, b, (size_t) n); }
 static int rawWrite (const char* b, int n)   { return (int) ::write (1, b, (size_t) n); }
#endif

static const char* engineVersion = "1.2.0";

//==============================================================================
static juce::CriticalSection outLock;
static void sendEvent (const juce::var& v)
{
    const auto line = juce::JSON::toString (v, true) + "\n";
    const juce::ScopedLock sl (outLock);
    const auto utf8 = line.toUTF8();
    const char* p = utf8.getAddress();
    int left = (int) std::strlen (p);
    while (left > 0)
    {
        const int w = rawWrite (p, left);
        if (w <= 0) break;
        p += w; left -= w;
    }
}

static juce::var obj (std::initializer_list<std::pair<const char*, juce::var>> props)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    for (auto& [k, v] : props) o->setProperty (k, v);
    return juce::var (o.get());
}

static juce::File appDataDir()
{
    auto d = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("JAJA Studio");
    d.createDirectory();
    return d;
}

//==============================================================================
class StdinReader : public juce::Thread
{
public:
    explicit StdinReader (std::function<void (juce::String)> onLine) : juce::Thread ("JAJA stdin"), handler (std::move (onLine)) {}
    void run() override
    {
       #if JUCE_WINDOWS
        _setmode (0, _O_BINARY); _setmode (1, _O_BINARY);
       #endif
        std::string pending;
        char buf[65536];
        while (! threadShouldExit())
        {
            const int n = rawRead (buf, (int) sizeof (buf));
            if (n <= 0) break;
            pending.append (buf, (size_t) n);
            size_t nl;
            while ((nl = pending.find ('\n')) != std::string::npos)
            {
                auto line = juce::String::fromUTF8 (pending.data(), (int) nl).trim();
                pending.erase (0, nl + 1);
                if (line.isNotEmpty())
                    juce::MessageManager::callAsync ([h = handler, line] { h (line); });
            }
        }
        juce::MessageManager::callAsync ([] { juce::JUCEApplicationBase::quit(); });
    }

private:
    std::function<void (juce::String)> handler;
};

//==============================================================================
class DeviceCallback : public juce::AudioIODeviceCallback
{
public:
    explicit DeviceCallback (jaja::Engine& e) : engine (e) {}
    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut, int n,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        engine.process (in, numIn, out, numOut, n);
    }
    void audioDeviceAboutToStart (juce::AudioIODevice* d) override
    {
        engine.prepare (d->getCurrentSampleRate(), juce::jmax (d->getCurrentBufferSizeSamples(), d->getDefaultBufferSize()) * 2);
    }
    void audioDeviceStopped() override {}
    void audioDeviceError (const juce::String& message) override
    {
        sendEvent (obj ({ { "ev", "deviceError" }, { "message", message } }));
    }

private:
    jaja::Engine& engine;
};

//==============================================================================
class PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow (std::shared_ptr<jaja::PluginSlot> s, std::function<void()> onClose)
        : juce::DocumentWindow (s->name + "  -  JAJA Studio", juce::Colour (0xff1d1d21), juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
          slot (std::move (s)), closed (std::move (onClose))
    {
        setUsingNativeTitleBar (true);
        juce::AudioProcessorEditor* ed = slot->instance->hasEditor() ? slot->instance->createEditorAndMakeActive() : nullptr;
        if (ed == nullptr) ed = new juce::GenericAudioProcessorEditor (*slot->instance);
        setContentOwned (ed, true);
        setResizable (ed->isResizable(), false);
        setAlwaysOnTop (true);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        toFront (true);
    }
    ~PluginWindow() override { clearContentComponent(); }
    void closeButtonPressed() override { if (closed) juce::MessageManager::callAsync (closed); }

    std::shared_ptr<jaja::PluginSlot> slot;

private:
    std::function<void()> closed;
};

//==============================================================================
class EngineApp : public juce::JUCEApplication, private juce::Timer
{
public:
    const juce::String getApplicationName() override    { return "JAJAEngine"; }
    const juce::String getApplicationVersion() override { return engineVersion; }
    bool moreThanOneInstanceAllowed() override          { return true; }

    void initialise (const juce::String&) override
    {
        addDefaultFormatsToManager (formats);
        if (auto xml = juce::parseXML (appDataDir().getChildFile ("plugins.xml"))) known.recreateFromXml (*xml);

        engine = std::make_unique<jaja::Engine>();
        engine->emit = sendEvent;
        engine->pluginFactory = [this] (const juce::String& pid, double sr, int bs, juce::String& err) -> std::unique_ptr<juce::AudioPluginInstance>
        {
            auto desc = known.getTypeForIdentifierString (pid);
            if (desc == nullptr) { err = "plugin not found - scan plugins again"; return nullptr; }
            return formats.createPluginInstance (*desc, sr, bs, err);
        };
        engine->onPluginRemoved = [this] (const juce::String& key) { windows.erase (key); };

        callback = std::make_unique<DeviceCallback> (*engine);
        openInitialDevice();
        deviceManager.addAudioCallback (callback.get());

        reader = std::make_unique<StdinReader> ([this] (juce::String line) { handleLine (line); });
        reader->startThread();
        startTimerHz (30);
        sendEvent (obj ({ { "ev", "hello" }, { "version", engineVersion }, { "device", deviceInfo() } }));
    }

    void shutdown() override
    {
        stopTimer();
        windows.clear();
        deviceManager.removeAudioCallback (callback.get());
        deviceManager.closeAudioDevice();
        engine.reset();
        if (reader) { reader->signalThreadShouldExit(); reader->stopThread (200); }
    }

    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted (const juce::String&) override {}

private:
    //==============================================================================
    void openInitialDevice()
    {
        auto saved = juce::parseXML (appDataDir().getChildFile ("engine-audio.xml"));
        const auto err = deviceManager.initialise (64, 2, saved.get(), true);
        if (saved == nullptr || err.isNotEmpty())
        {
            // first start: prefer an ASIO driver if one is installed (e.g. the Behringer UMC driver)
            for (auto* type : deviceManager.getAvailableDeviceTypes())
                if (type->getTypeName() == "ASIO")
                {
                    type->scanForDevices();
                    const auto names = type->getDeviceNames();
                    if (names.isEmpty()) break;
                    juce::String pick = names[0];
                    for (auto& n : names) if (n.containsIgnoreCase ("UMC") || n.containsIgnoreCase ("Behringer")) pick = n;
                    applyDevice ("ASIO", pick, pick, 0, 0);
                    break;
                }
        }
    }

    juce::String applyDevice (const juce::String& type, const juce::String& output, const juce::String& input, double sampleRate, int bufferSize)
    {
        if (type.isNotEmpty() && type != deviceManager.getCurrentAudioDeviceType())
            deviceManager.setCurrentAudioDeviceType (type, true);
        auto setup = deviceManager.getAudioDeviceSetup();
        const bool asio = deviceManager.getCurrentAudioDeviceType() == "ASIO";
        if (output.isNotEmpty()) setup.outputDeviceName = output;
        setup.inputDeviceName = asio ? setup.outputDeviceName : (input == "-" ? juce::String() : (input.isNotEmpty() ? input : setup.inputDeviceName));
        if (sampleRate > 0) setup.sampleRate = sampleRate;
        if (bufferSize > 0) setup.bufferSize = bufferSize;
        setup.useDefaultInputChannels = false; setup.inputChannels.clear(); setup.inputChannels.setRange (0, 64, true);
        setup.useDefaultOutputChannels = false; setup.outputChannels.clear(); setup.outputChannels.setRange (0, 2, true);
        const auto err = deviceManager.setAudioDeviceSetup (setup, true);
        if (auto xml = deviceManager.createStateXml()) xml->writeTo (appDataDir().getChildFile ("engine-audio.xml"));
        return err;
    }

    juce::var deviceInfo()
    {
        juce::Array<juce::var> types;
        for (auto* type : deviceManager.getAvailableDeviceTypes())
        {
            type->scanForDevices();
            juce::Array<juce::var> outs, ins;
            for (auto& n : type->getDeviceNames (false)) outs.add (n);
            for (auto& n : type->getDeviceNames (true)) ins.add (n);
            types.add (obj ({ { "name", type->getTypeName() }, { "outputs", outs }, { "inputs", ins } }));
        }
        juce::var cur;
        if (auto* d = deviceManager.getCurrentAudioDevice())
        {
            juce::Array<juce::var> rates, sizes, inNames, outNames;
            for (auto r : d->getAvailableSampleRates()) rates.add (r);
            for (auto s : d->getAvailableBufferSizes()) sizes.add (s);
            for (auto& n : d->getInputChannelNames()) inNames.add (n);
            for (auto& n : d->getOutputChannelNames()) outNames.add (n);
            const auto setup = deviceManager.getAudioDeviceSetup();
            cur = obj ({ { "type", d->getTypeName() }, { "name", d->getName() },
                         { "output", setup.outputDeviceName }, { "input", setup.inputDeviceName },
                         { "sampleRate", d->getCurrentSampleRate() }, { "bufferSize", d->getCurrentBufferSizeSamples() },
                         { "sampleRates", rates }, { "bufferSizes", sizes },
                         { "inputs", inNames }, { "outputs", outNames },
                         { "inputLatency", d->getInputLatencyInSamples() }, { "outputLatency", d->getOutputLatencyInSamples() },
                         { "hasPanel", d->hasControlPanel() } });
        }
        return obj ({ { "types", types }, { "current", cur } });
    }

    juce::var pluginList()
    {
        juce::Array<juce::var> list;
        for (auto& d : known.getTypes())
            list.add (obj ({ { "pid", d.createIdentifierString() }, { "name", d.name }, { "vendor", d.manufacturerName },
                             { "instrument", d.isInstrument }, { "format", d.pluginFormatName }, { "category", d.category } }));
        return list;
    }

    //==============================================================================
    void reply (const juce::var& msg, juce::var payload)
    {
        if (! msg["rid"].isVoid())
        {
            if (! payload.isObject()) payload = obj ({ { "value", payload } });
            payload.getDynamicObject()->setProperty ("ev", "reply");
            payload.getDynamicObject()->setProperty ("rid", msg["rid"]);
            sendEvent (payload);
        }
    }

    void handleLine (const juce::String& line)
    {
        const auto msg = juce::JSON::parse (line);
        if (! msg.isObject()) return;
        const auto cmd = msg["cmd"].toString();
        try { handle (cmd, msg); }
        catch (const std::exception& e) { sendEvent (obj ({ { "ev", "error" }, { "cmd", cmd }, { "message", juce::String (e.what()) } })); }
    }

    void handle (const juce::String& cmd, const juce::var& msg)
    {
        if (cmd == "hello")          reply (msg, obj ({ { "version", engineVersion }, { "device", deviceInfo() } }));
        else if (cmd == "devices")   reply (msg, deviceInfo());
        else if (cmd == "setDevice")
        {
            const auto err = applyDevice (msg["type"].toString(), msg["output"].toString(), msg["input"].toString(),
                                          (double) msg.getProperty ("sampleRate", 0), (int) msg.getProperty ("bufferSize", 0));
            auto info = deviceInfo();
            if (err.isNotEmpty()) info.getDynamicObject()->setProperty ("error", err);
            reply (msg, info);
            sendEvent (obj ({ { "ev", "device" }, { "device", info } }));
        }
        else if (cmd == "asioPanel")
        {
            if (auto* d = deviceManager.getCurrentAudioDevice(); d != nullptr && d->hasControlPanel())
            {
                bool changed;
                {
                    juce::Component modal;   // same pattern as JUCE's own device selector
                    modal.setOpaque (true);
                    modal.addToDesktop (0);
                    modal.enterModalState();
                    changed = d->showControlPanel();
                }
                if (changed) { deviceManager.closeAudioDevice(); deviceManager.restartLastAudioDevice(); }
            }
            const auto info = deviceInfo();
            reply (msg, info);
            sendEvent (obj ({ { "ev", "device" }, { "device", info } }));
        }
        else if (cmd == "project")   engine->setProject (msg["project"]);
        else if (cmd == "audio")
        {
            juce::String err;
            const bool ok = engine->loadMedia (msg["id"].toString(), juce::File (msg["path"].toString()), err);
            reply (msg, obj ({ { "ok", ok }, { "error", err } }));
        }
        else if (cmd == "play")      engine->play ((double) msg.getProperty ("beat", 0.0), (double) msg.getProperty ("clickUntil", -1.0e9));
        else if (cmd == "locate")    engine->locate ((double) msg.getProperty ("beat", 0.0));
        else if (cmd == "stop")
        {
            if (engine->isRecording()) sendEvent (obj ({ { "ev", "recorded" }, { "takes", engine->stopRecord() } }));
            else engine->stop();
            sendEvent (engine->takeMeters());
        }
        else if (cmd == "record")
        {
            int latency = 0;
            if (auto* d = deviceManager.getCurrentAudioDevice()) latency = d->getInputLatencyInSamples() + d->getOutputLatencyInSamples();
            auto folder = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("JAJA Studio").getChildFile ("Recordings");
            juce::String err;
            juce::Array<juce::var> ids;
            if (auto* a = msg["tracks"].getArray()) ids = *a;
            const bool ok = engine->startRecord (ids, (double) msg.getProperty ("startBeat", 0.0), (bool) msg.getProperty ("countIn", false), folder, latency, err);
            reply (msg, obj ({ { "ok", ok }, { "error", err }, { "latency", latency } }));
        }
        else if (cmd == "note")      engine->liveNote (msg["track"].toString(), (int) msg["p"], (int) msg.getProperty ("v", 100));
        else if (cmd == "plugins")   reply (msg, obj ({ { "plugins", pluginList() } }));
        else if (cmd == "scan")      startScan ((bool) msg.getProperty ("rescan", false), msg["paths"]);
        else if (cmd == "editor")    openEditor (msg["key"].toString());
        else if (cmd == "closeEditor") windows.erase (msg["key"].toString());
        else if (cmd == "states")    reply (msg, obj ({ { "states", engine->pluginStates() } }));
        else if (cmd == "bounce")    startBounce (msg);
        else if (cmd == "quit")      quit();
    }

    //==============================================================================
    void openEditor (const juce::String& key)
    {
        if (auto it = windows.find (key); it != windows.end()) { it->second->setMinimised (false); it->second->toFront (true); return; }
        auto slot = engine->findPlugin (key);
        if (slot == nullptr || slot->instance == nullptr) { sendEvent (obj ({ { "ev", "error" }, { "message", "plugin is not loaded: " + key } })); return; }
        windows[key] = std::make_unique<PluginWindow> (slot, [this, key]
        {
            if (auto it = windows.find (key); it != windows.end())
            {
                juce::MemoryBlock mb;
                it->second->slot->instance->getStateInformation (mb);
                sendEvent (obj ({ { "ev", "pluginState" }, { "key", key }, { "state", mb.toBase64Encoding() } }));
                windows.erase (it);
            }
        });
    }

    void startScan (bool rescan, const juce::var& extraPaths)
    {
        if (scanner) return;
        if (rescan) known.clear();
        juce::AudioPluginFormat* vst3 = nullptr;
        for (auto* f : formats.getFormats()) if (f->getName() == "VST3") vst3 = f;
        if (vst3 == nullptr) { sendEvent (obj ({ { "ev", "scanDone" }, { "plugins", pluginList() }, { "message", "VST3 hosting is not available" } })); return; }
        auto paths = vst3->getDefaultLocationsToSearch();
        if (auto* a = extraPaths.getArray()) for (auto& p : *a) paths.add (juce::File (p.toString()));
        scanner = std::make_unique<juce::PluginDirectoryScanner> (known, *vst3, paths, true, appDataDir().getChildFile ("scan-in-progress.txt"), false);
        scanTimer.startTimer (5);
    }

    void scanStep()
    {
        if (! scanner) { scanTimer.stopTimer(); return; }
        juce::String name;
        const bool more = scanner->scanNextFile (true, name);
        sendEvent (obj ({ { "ev", "scan" }, { "name", name }, { "progress", scanner->getProgress() } }));
        if (more) return;
        scanTimer.stopTimer();
        juce::Array<juce::var> failed;
        for (auto& f : scanner->getFailedFiles()) failed.add (f);
        scanner.reset();
        if (auto xml = known.createXml()) xml->writeTo (appDataDir().getChildFile ("plugins.xml"));
        sendEvent (obj ({ { "ev", "scanDone" }, { "plugins", pluginList() }, { "failed", failed } }));
    }

    void startBounce (const juce::var& msg)
    {
        if (bouncing) return;
        bouncing = true;
        const juce::File file (msg["path"].toString());
        const double endBeat = (double) msg.getProperty ("endBeat", 0.0), tail = (double) msg.getProperty ("tail", 3.0);
        const auto rid = msg["rid"];
        const juce::String onlyTrack = msg["track"].toString();   // stem export: one track at a time
        juce::Thread::launch ([this, file, endBeat, tail, rid, onlyTrack]
        {
            juce::String err;
            const bool ok = engine->bounce (file, endBeat, tail, [] (float p) { sendEvent (obj ({ { "ev", "bounce" }, { "progress", p } })); }, err, onlyTrack);
            sendEvent (obj ({ { "ev", "reply" }, { "rid", rid }, { "ok", ok }, { "error", err }, { "path", file.getFullPathName() } }));
            bouncing = false;
        });
    }

    void timerCallback() override { if (! bouncing) sendEvent (engine->takeMeters()); }

    struct ScanTimer : juce::Timer
    {
        std::function<void()> fn;
        void timerCallback() override { fn(); }
    };

    juce::AudioDeviceManager deviceManager;
    juce::AudioPluginFormatManager formats;
    juce::KnownPluginList known;
    std::unique_ptr<juce::PluginDirectoryScanner> scanner;
    ScanTimer scanTimer;
    std::unique_ptr<jaja::Engine> engine;
    std::unique_ptr<DeviceCallback> callback;
    std::unique_ptr<StdinReader> reader;
    std::map<juce::String, std::unique_ptr<PluginWindow>> windows;
    std::atomic<bool> bouncing { false };

public:
    EngineApp() { scanTimer.fn = [this] { scanStep(); }; }
};

START_JUCE_APPLICATION (EngineApp)
