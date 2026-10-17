// JAJA Engine — built-in instruments (ports of the JAJA Studio web synths)
#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <array>
#include <vector>
#include <cmath>
#include <map>
#include <random>

namespace jaja
{
//==============================================================================
struct InstDef
{
    struct Osc { int wave; float detuneCents; float octave; float mix; };   // wave: 0 sine 1 tri 2 saw 3 square
    std::vector<Osc> osc;
    float a, d, s, r, cutoff, q, fenv, gain;
};

inline const InstDef& instDef (const juce::String& key)
{
    static const std::map<juce::String, InstDef> defs {
        { "keys",  { { { 1, 0, 0, 0.8f }, { 0, 0, 12, 0.25f } },                   0.004f, 0.7f,  0.35f, 0.35f, 3800, 0.5f, 1.0f, 0.32f } },
        { "pad",   { { { 2, -9, 0, 0.5f }, { 2, 9, 0, 0.5f }, { 1, 0, -12, 0.3f } }, 0.35f,  1.2f,  0.8f,  0.9f,  1700, 0.6f, 1.0f, 0.18f } },
        { "bass",  { { { 2, 0, 0, 0.6f }, { 0, 0, -12, 0.9f } },                   0.004f, 0.25f, 0.65f, 0.1f,  700,  2.0f, 2.2f, 0.42f } },
        { "lead",  { { { 3, 0, 0, 0.45f }, { 2, 6, 0, 0.45f } },                   0.01f,  0.25f, 0.7f,  0.2f,  2600, 1.5f, 1.0f, 0.2f } },
        { "pluck", { { { 2, 0, 0, 0.6f }, { 1, 0, 12, 0.3f } },                    0.002f, 0.28f, 0.0f,  0.25f, 2400, 3.0f, 3.0f, 0.3f } },
    };
    auto it = defs.find (key);
    return it != defs.end() ? it->second : defs.at ("keys");
}

//==============================================================================
/** Topology-preserving state-variable low-pass filter. */
struct SvfLowpass
{
    float ic1 = 0, ic2 = 0;
    void reset() { ic1 = ic2 = 0; }
    float process (float x, float cutoff, float q, float sr)
    {
        const float g = std::tan (juce::MathConstants<float>::pi * juce::jlimit (20.0f, sr * 0.45f, cutoff) / sr);
        const float k = 1.0f / juce::jmax (0.05f, q);
        const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
        const float v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2;
        return v2;
    }
};

/** RBJ biquad, used by the drum kit. */
struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void set (int type, float f, float q, float sr)   // 0 low-pass 1 high-pass 2 band-pass
    {
        const float w = juce::MathConstants<float>::twoPi * juce::jlimit (10.0f, sr * 0.45f, f) / sr, cs = std::cos (w), al = std::sin (w) / (2 * q);
        float nb0, nb1, nb2;
        if (type == 0)      { nb0 = (1 - cs) / 2; nb1 = 1 - cs;    nb2 = (1 - cs) / 2; }
        else if (type == 1) { nb0 = (1 + cs) / 2; nb1 = -(1 + cs); nb2 = (1 + cs) / 2; }
        else                { nb0 = al;           nb1 = 0;         nb2 = -al; }
        const float a0 = 1 + al;
        b0 = nb0 / a0; b1 = nb1 / a0; b2 = nb2 / a0; a1 = -2 * cs / a0; a2 = (1 - al) / a0;
    }
    float process (float x) { const float y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
};

inline float polyBlep (double t, double dt)
{
    if (t < dt)        { t /= dt; return (float) (t + t - t * t - 1.0); }
    if (t > 1.0 - dt)  { t = (t - 1.0) / dt; return (float) (t * t + t + t + 1.0); }
    return 0.0f;
}

//==============================================================================
class BuiltinSynth
{
public:
    void setInstrument (const juce::String& key) { def = &instDef (key); }
    void prepare (double sampleRate) { sr = (float) sampleRate; allNotesOff(); }
    void allNotesOff() { for (auto& v : voices) v.stage = Voice::Off; }

    void render (juce::AudioBuffer<float>& buf, const juce::MidiBuffer& midi, int numSamples)
    {
        int pos = 0;
        for (const auto meta : midi)
        {
            const int at = juce::jlimit (0, numSamples, meta.samplePosition);
            renderVoices (buf, pos, at - pos); pos = at;
            handle (meta.getMessage());
        }
        renderVoices (buf, pos, numSamples - pos);
    }

private:
    struct Voice
    {
        enum Stage { Off, Attack, Decay, Release } stage = Off;
        int note = -1; float amp = 0, level = 0, t = 0, vel = 1;
        std::array<double, 3> phase {};
        SvfLowpass filter;
    };

    void handle (const juce::MidiMessage& m)
    {
        if (m.isNoteOn())
        {
            auto* v = &voices[0];
            for (auto& x : voices) if (x.stage == Voice::Off) { v = &x; break; }
            if (v->stage != Voice::Off)   // steal the quietest voice
                for (auto& x : voices) if (x.level < v->level) v = &x;
            v->stage = Voice::Attack; v->note = m.getNoteNumber(); v->vel = m.getFloatVelocity();
            v->amp = v->vel * def->gain; v->level = 0; v->t = 0; v->filter.reset();
            for (auto& p : v->phase) p = 0;
        }
        else if (m.isNoteOff())
        {
            for (auto& v : voices) if (v.note == m.getNoteNumber() && v.stage != Voice::Off && v.stage != Voice::Release) v.stage = Voice::Release;
        }
        else if (m.isAllNotesOff() || m.isAllSoundOff())
        {
            for (auto& v : voices) if (v.stage != Voice::Off) v.stage = Voice::Release;
        }
    }

    void renderVoices (juce::AudioBuffer<float>& buf, int start, int num)
    {
        if (num <= 0) return;
        auto* L = buf.getWritePointer (0, start);
        auto* R = buf.getNumChannels() > 1 ? buf.getWritePointer (1, start) : nullptr;
        const InstDef& I = *def;
        const float dCoef = std::exp (-1.0f / (juce::jmax (0.001f, I.d / 3) * sr));
        const float rCoef = std::exp (-1.0f / (juce::jmax (0.001f, I.r / 3) * sr));
        for (auto& v : voices)
        {
            if (v.stage == Voice::Off) continue;
            const double f0 = 440.0 * std::pow (2.0, (v.note - 69) / 12.0);
            const float cutBase = juce::jmin (16000.0f, I.cutoff * (1.0f + (v.note - 60) / 60.0f));
            std::array<double, 3> inc {};
            for (size_t o = 0; o < I.osc.size() && o < 3; ++o)
                inc[o] = f0 * std::pow (2.0, I.osc[o].octave / 12.0 + I.osc[o].detuneCents / 1200.0) / sr;
            for (int i = 0; i < num; ++i)
            {
                switch (v.stage)
                {
                    case Voice::Attack:  v.level += v.amp / juce::jmax (1.0f, I.a * sr); if (v.level >= v.amp) { v.level = v.amp; v.stage = Voice::Decay; } break;
                    case Voice::Decay:   { const float target = v.amp * I.s + 0.0001f; v.level = target + (v.level - target) * dCoef; break; }
                    case Voice::Release: v.level *= rCoef; if (v.level < 0.00002f) { v.stage = Voice::Off; v.note = -1; } break;
                    case Voice::Off: break;
                }
                if (v.stage == Voice::Off) break;
                float s = 0;
                for (size_t o = 0; o < I.osc.size() && o < 3; ++o)
                {
                    const double p = v.phase[o], dt = inc[o];
                    float w;
                    switch (I.osc[o].wave)
                    {
                        case 0:  w = (float) std::sin (juce::MathConstants<double>::twoPi * p); break;
                        case 1:  w = (float) (p < 0.5 ? 4 * p - 1 : 3 - 4 * p); break;
                        case 2:  w = (float) (2 * p - 1) - polyBlep (p, dt); break;
                        default: w = (p < 0.5 ? 1.0f : -1.0f) + polyBlep (p, dt) - polyBlep (std::fmod (p + 0.5, 1.0), dt); break;
                    }
                    s += w * I.osc[o].mix;
                    v.phase[o] = p + dt; if (v.phase[o] >= 1.0) v.phase[o] -= 1.0;
                }
                const float cut = I.fenv > 1.0f ? cutBase + (cutBase * I.fenv - cutBase) * std::exp (-v.t / juce::jmax (0.001f, I.d / 2)) : cutBase;
                const float y = v.filter.process (s, cut, I.q, sr) * v.level;
                L[i] += y; if (R) R[i] += y;
                v.t += 1.0f / sr;
            }
        }
    }

    const InstDef* def = &instDef ("keys");
    float sr = 44100;
    std::array<Voice, 24> voices;
};

//==============================================================================
class DrumKit
{
public:
    void prepare (double sampleRate) { sr = (float) sampleRate; hits.clear(); hits.reserve (64); }
    void allNotesOff() {}

    void render (juce::AudioBuffer<float>& buf, const juce::MidiBuffer& midi, int numSamples)
    {
        int pos = 0;
        for (const auto meta : midi)
        {
            const int at = juce::jlimit (0, numSamples, meta.samplePosition);
            renderHits (buf, pos, at - pos); pos = at;
            const auto m = meta.getMessage();
            if (m.isNoteOn()) trigger (m.getNoteNumber(), m.getFloatVelocity());
        }
        renderHits (buf, pos, numSamples - pos);
    }

private:
    struct Part { int kind; float peak, decay, f0, f1, sweep; int filt; float ff, q; float delay; };   // kind: 0 tone 1 noise 2 square
    struct Hit { std::vector<Part> parts; float vel; double t; double phase[4] {}; Biquad bq[4]; };

    void trigger (int note, float vel)
    {
        Hit h; h.vel = vel; h.t = 0;
        switch (note)
        {
            case 36: h.parts = { { 0, 1.0f, 0.45f, 150, 45, 0.12f, 0, 0, 0, 0 }, { 1, 0.25f, 0.02f, 0, 0, 0, 1, 3000, 0.7f, 0 } }; break;
            case 38: h.parts = { { 0, 0.45f, 0.12f, 220, 160, 0.08f, 0, 0, 0, 0 }, { 1, 0.55f, 0.2f, 0, 0, 0, 1, 1500, 0.7f, 0 } }; break;
            case 39: h.parts = { { 1, 0.6f, 0.01f, 0, 0, 0, 2, 1300, 1.2f, 0 }, { 1, 0.6f, 0.01f, 0, 0, 0, 2, 1300, 1.2f, 0.012f }, { 1, 0.6f, 0.18f, 0, 0, 0, 2, 1300, 1.2f, 0.024f } }; break;
            case 42: h.parts = { { 1, 0.32f, 0.05f, 0, 0, 0, 1, 7500, 0.7f, 0 } }; break;
            case 46: h.parts = { { 1, 0.28f, 0.32f, 0, 0, 0, 1, 6500, 0.7f, 0 } }; break;
            case 49: h.parts = { { 1, 0.25f, 1.3f, 0, 0, 0, 1, 4500, 0.7f, 0 } }; break;
            case 37: h.parts = { { 2, 0.18f, 0.03f, 1700, 1700, 0, 0, 0, 0, 0 } }; break;
            default: h.parts = { { 0, 0.7f, 0.3f, 190, 95, 0.25f, 0, 0, 0, 0 } }; break;
        }
        for (size_t i = 0; i < h.parts.size() && i < 4; ++i)
            if (h.parts[i].kind == 1) h.bq[i].set (h.parts[i].filt == 1 ? 1 : 2, h.parts[i].ff, h.parts[i].q, sr);
        if (hits.size() > 48) hits.erase (hits.begin());
        hits.push_back (std::move (h));
    }

    void renderHits (juce::AudioBuffer<float>& buf, int start, int num)
    {
        if (num <= 0) return;
        auto* L = buf.getWritePointer (0, start);
        auto* R = buf.getNumChannels() > 1 ? buf.getWritePointer (1, start) : nullptr;
        for (auto& h : hits)
        {
            for (int i = 0; i < num; ++i)
            {
                float s = 0;
                for (size_t k = 0; k < h.parts.size() && k < 4; ++k)
                {
                    auto& p = h.parts[k];
                    const double t = h.t - p.delay;
                    if (t < 0 || t > p.decay + 0.02) continue;
                    const float env = p.peak * h.vel * std::pow (0.0001f / p.peak, (float) (t / p.decay));
                    float w;
                    if (p.kind == 1) w = h.bq[k].process (noise() * 1.0f);
                    else
                    {
                        const double f = p.sweep > 0 ? p.f0 * std::pow ((double) p.f1 / p.f0, juce::jmin (1.0, t / p.sweep)) : p.f0;
                        h.phase[k] += f / sr; if (h.phase[k] >= 1) h.phase[k] -= 1;
                        w = p.kind == 2 ? (h.phase[k] < 0.5 ? 1.0f : -1.0f) : (float) std::sin (juce::MathConstants<double>::twoPi * h.phase[k]);
                    }
                    s += w * env;
                }
                L[i] += s; if (R) R[i] += s;
                h.t += 1.0 / sr;
            }
        }
        hits.erase (std::remove_if (hits.begin(), hits.end(), [] (const Hit& h)
        {
            for (auto& p : h.parts) if (h.t < p.delay + p.decay + 0.03) return false;
            return true;
        }), hits.end());
    }

    float noise() { seed = seed * 1664525u + 1013904223u; return (float) (seed >> 8) / 8388608.0f - 1.0f; }

    float sr = 44100;
    uint32_t seed = 12345;
    std::vector<Hit> hits;
};
} // namespace jaja
