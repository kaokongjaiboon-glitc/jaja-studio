// JAJA Engine — built-in effects (same names and parameters as the JAJA Studio web effects)
#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>
#include <memory>
#include <vector>

namespace jaja
{
inline float dbToGain (float db) { return std::pow (10.0f, db / 20.0f); }
inline float param (const juce::var& p, const char* key, float def)
{
    const auto v = p.getProperty (juce::Identifier (key), juce::var());
    return v.isVoid() ? def : (float) (double) v;
}

/** Base class for an effect working on a stereo pair. */
struct Fx
{
    virtual ~Fx() = default;
    virtual void prepare (double sampleRate, int maxBlock) = 0;
    virtual void setParams (const juce::var& p, double bpm) = 0;
    virtual void process (float* L, float* R, int n) = 0;
    virtual void reset() {}
};

//==============================================================================
/** RBJ biquad with the same filter shapes as the Web Audio BiquadFilterNode. */
struct Rbj
{
    enum Type { lowpass, highpass, lowshelf, highshelf, peaking, bandpass };
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1[2] {}, z2[2] {};

    void set (Type t, double sr, double f, double q, double gainDb)
    {
        f = juce::jlimit (10.0, sr * 0.49, f);
        const double A = std::pow (10.0, gainDb / 40.0), w = 2 * juce::MathConstants<double>::pi * f / sr;
        const double cs = std::cos (w), sn = std::sin (w);
        double alpha = sn / (2 * juce::jmax (0.0001, q));
        double B0, B1, B2, A0, A1, A2;
        switch (t)
        {
            case lowpass:  B0 = (1 - cs) / 2; B1 = 1 - cs; B2 = (1 - cs) / 2; A0 = 1 + alpha; A1 = -2 * cs; A2 = 1 - alpha; break;
            case highpass: B0 = (1 + cs) / 2; B1 = -(1 + cs); B2 = (1 + cs) / 2; A0 = 1 + alpha; A1 = -2 * cs; A2 = 1 - alpha; break;
            case bandpass: B0 = alpha; B1 = 0; B2 = -alpha; A0 = 1 + alpha; A1 = -2 * cs; A2 = 1 - alpha; break;
            case peaking:  B0 = 1 + alpha * A; B1 = -2 * cs; B2 = 1 - alpha * A; A0 = 1 + alpha / A; A1 = -2 * cs; A2 = 1 - alpha / A; break;
            case lowshelf:
            {
                alpha = sn / 2 * std::sqrt (2.0);   // shelf slope 1, as in Web Audio
                const double k = 2 * std::sqrt (A) * alpha;
                B0 = A * ((A + 1) - (A - 1) * cs + k); B1 = 2 * A * ((A - 1) - (A + 1) * cs); B2 = A * ((A + 1) - (A - 1) * cs - k);
                A0 = (A + 1) + (A - 1) * cs + k; A1 = -2 * ((A - 1) + (A + 1) * cs); A2 = (A + 1) + (A - 1) * cs - k; break;
            }
            case highshelf:
            default:
            {
                alpha = sn / 2 * std::sqrt (2.0);
                const double k = 2 * std::sqrt (A) * alpha;
                B0 = A * ((A + 1) + (A - 1) * cs + k); B1 = -2 * A * ((A - 1) + (A + 1) * cs); B2 = A * ((A + 1) + (A - 1) * cs - k);
                A0 = (A + 1) - (A - 1) * cs + k; A1 = 2 * ((A - 1) - (A + 1) * cs); A2 = (A + 1) - (A - 1) * cs - k; break;
            }
        }
        b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
    }
    inline float tick (float x, int ch)
    {
        const double y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return (float) y;
    }
    void process (float* L, float* R, int n) { for (int i = 0; i < n; ++i) { L[i] = tick (L[i], 0); R[i] = tick (R[i], 1); } }
    void reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0; }
};

/** One-pole smoother / low-pass used by the delay and saturator tone controls. */
struct OnePole
{
    float a = 0, z[2] {};
    void set (float f, float sr) { a = std::exp (-juce::MathConstants<float>::twoPi * juce::jlimit (20.0f, sr * 0.49f, f) / sr); }
    inline float tick (float x, int ch) { z[ch] = x * (1 - a) + z[ch] * a; return z[ch]; }
};

//==============================================================================
struct EqFx : Fx
{
    double sr = 44100; Rbj hp, ls, pk, hs; bool hpOn = false;
    void prepare (double s, int) override { sr = s; reset(); }
    void setParams (const juce::var& p, double) override
    {
        const float hpf = param (p, "hpf", 20);
        hpOn = hpf > 20.5f;
        hp.set (Rbj::highpass, sr, hpf, 0.707, 0);
        ls.set (Rbj::lowshelf, sr, param (p, "lf", 120), 1, param (p, "lg", 0));
        pk.set (Rbj::peaking, sr, param (p, "mf", 1000), param (p, "mq", 1), param (p, "mg", 0));
        hs.set (Rbj::highshelf, sr, param (p, "hf", 8000), 1, param (p, "hg", 0));
    }
    void process (float* L, float* R, int n) override { if (hpOn) hp.process (L, R, n); ls.process (L, R, n); pk.process (L, R, n); hs.process (L, R, n); }
    void reset() override { hp.reset(); ls.reset(); pk.reset(); hs.reset(); }
};

/** Feed-forward stereo-linked compressor with a soft knee. */
struct CompFx : Fx
{
    float sr = 44100, thr = -20, ratio = 4, knee = 6, makeup = 1, attC = 0, relC = 0, env = 0;
    void prepare (double s, int) override { sr = (float) s; env = 0; }
    void setParams (const juce::var& p, double) override
    {
        thr = param (p, "thr", -20); ratio = juce::jmax (1.0f, param (p, "ratio", 4)); knee = param (p, "knee", 6);
        makeup = dbToGain (param (p, "makeup", 3));
        attC = std::exp (-1.0f / (juce::jmax (0.05f, param (p, "att", 10)) * 0.001f * sr));
        relC = std::exp (-1.0f / (juce::jmax (5.0f, param (p, "rel", 150)) * 0.001f * sr));
    }
    float gainFor (float levelDb) const
    {
        const float over = levelDb - thr;
        float outDb;
        if (knee > 0 && std::abs (over) <= knee / 2) outDb = levelDb + (1 / ratio - 1) * (over + knee / 2) * (over + knee / 2) / (2 * knee);
        else if (over > 0) outDb = thr + over / ratio;
        else outDb = levelDb;
        return outDb - levelDb;
    }
    void process (float* L, float* R, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            const float x = juce::jmax (std::abs (L[i]), std::abs (R[i]));
            env = x > env ? x + (env - x) * attC : x + (env - x) * relC;
            const float g = dbToGain (gainFor (juce::Decibels::gainToDecibels (env, -120.0f))) * makeup;
            L[i] *= g; R[i] *= g;
        }
    }
    void reset() override { env = 0; }
};

/** Stereo Freeverb-style reverb with pre-delay and damping, wet/dry like the web version. */
struct ReverbFx : Fx
{
    double sr = 44100; juce::Reverb verb; std::vector<float> pre[2]; int prePos = 0, preLen = 1;
    OnePole damp; float wet = 0.25f, dry = 0.875f; std::vector<float> wl, wr;
    void prepare (double s, int maxBlock) override
    {
        sr = s; verb.setSampleRate (s);
        for (auto& b : pre) b.assign ((size_t) (s * 0.13) + 2, 0.0f);
        wl.assign ((size_t) maxBlock, 0); wr.assign ((size_t) maxBlock, 0);
    }
    void setParams (const juce::var& p, double) override
    {
        juce::Reverb::Parameters rp;
        const float size = param (p, "size", 2.4f);
        rp.roomSize = juce::jlimit (0.1f, 0.98f, 0.45f + std::log (size / 0.3f) / std::log (8.0f / 0.3f) * 0.53f);
        rp.damping = juce::jlimit (0.0f, 1.0f, 1.0f - std::log (param (p, "damp", 6000) / 1500.0f) / std::log (16000.0f / 1500.0f));
        rp.wetLevel = 1; rp.dryLevel = 0; rp.width = 1;
        verb.setParameters (rp);
        preLen = juce::jlimit (1, (int) pre[0].size() - 1, (int) (param (p, "pre", 15) * 0.001 * sr) + 1);
        damp.set (param (p, "damp", 6000), (float) sr);
        const float mix = param (p, "mix", 25) / 100.0f;
        wet = mix * 0.6f; dry = 1 - mix * 0.5f;
    }
    void process (float* L, float* R, int n) override
    {
        if ((int) wl.size() < n) { wl.resize ((size_t) n); wr.resize ((size_t) n); }
        const int len = (int) pre[0].size();
        for (int i = 0; i < n; ++i)
        {
            const int rd = (prePos - preLen + len) % len;
            pre[0][(size_t) prePos] = L[i]; pre[1][(size_t) prePos] = R[i];
            wl[(size_t) i] = pre[0][(size_t) rd]; wr[(size_t) i] = pre[1][(size_t) rd];
            prePos = (prePos + 1) % len;
        }
        verb.processStereo (wl.data(), wr.data(), n);
        for (int i = 0; i < n; ++i)
        {
            L[i] = L[i] * dry + damp.tick (wl[(size_t) i], 0) * wet;
            R[i] = R[i] * dry + damp.tick (wr[(size_t) i], 1) * wet;
        }
    }
    void reset() override { verb.reset(); for (auto& b : pre) std::fill (b.begin(), b.end(), 0.0f); }
};

/** Tempo-synced feedback delay with a tone filter in the loop. */
struct DelayFx : Fx
{
    double sr = 44100; std::vector<float> buf[2]; int pos = 0, dly = 1; float fb = 0.35f, wet = 0.25f, dry = 0.9f; OnePole lp;
    void prepare (double s, int) override { sr = s; for (auto& b : buf) b.assign ((size_t) (s * 4.0) + 2, 0.0f); pos = 0; }
    void setParams (const juce::var& p, double bpm) override
    {
        const double secs = juce::jlimit (0.01, 3.9, (double) param (p, "time", 0.75f) * 60.0 / bpm);
        dly = juce::jlimit (1, (int) buf[0].size() - 1, (int) (secs * sr));
        fb = param (p, "fb", 35) / 100.0f; lp.set (param (p, "tone", 5000), (float) sr);
        const float mix = param (p, "mix", 25) / 100.0f; wet = mix; dry = 1 - mix * 0.4f;
    }
    void process (float* L, float* R, int n) override
    {
        const int len = (int) buf[0].size();
        for (int i = 0; i < n; ++i)
        {
            const int rd = (pos - dly + len) % len;
            const float yl = lp.tick (buf[0][(size_t) rd], 0), yr = lp.tick (buf[1][(size_t) rd], 1);
            buf[0][(size_t) pos] = L[i] + yl * fb; buf[1][(size_t) pos] = R[i] + yr * fb;
            L[i] = L[i] * dry + yl * wet; R[i] = R[i] * dry + yr * wet;
            pos = (pos + 1) % len;
        }
    }
    void reset() override { for (auto& b : buf) std::fill (b.begin(), b.end(), 0.0f); }
};

/** Two modulated delay lines panned left and right. */
struct ChorusFx : Fx
{
    double sr = 44100; std::vector<float> buf; int pos = 0; double ph[2] { 0, 0.25 }; float rate = 0.8f, depth = 0.003f, wet = 0.45f, dry = 0.78f;
    void prepare (double s, int) override { sr = s; buf.assign ((size_t) (s * 0.05) + 4, 0.0f); pos = 0; }
    void setParams (const juce::var& p, double) override
    {
        rate = param (p, "rate", 0.8f); depth = param (p, "depth", 3) / 1000.0f;
        const float mix = param (p, "mix", 45) / 100.0f; wet = mix; dry = 1 - mix * 0.5f;
    }
    void process (float* L, float* R, int n) override
    {
        const int len = (int) buf.size();
        const float gBig = std::cos (0.2f * juce::MathConstants<float>::halfPi), gSmall = std::sin (0.2f * juce::MathConstants<float>::halfPi);   // pan -0.8 / +0.8
        for (int i = 0; i < n; ++i)
        {
            buf[(size_t) pos] = (L[i] + R[i]) * 0.5f;
            float v[2];
            for (int k = 0; k < 2; ++k)
            {
                const double d = (0.012 + depth * std::sin (juce::MathConstants<double>::twoPi * ph[k])) * sr;
                ph[k] += rate * (k ? 1.13 : 1.0) / sr; if (ph[k] >= 1) ph[k] -= 1;
                double rp = pos - d; while (rp < 0) rp += len;
                const int i0 = (int) rp, i1 = (i0 + 1) % len; const float fr = (float) (rp - i0);
                v[k] = buf[(size_t) i0] + (buf[(size_t) i1] - buf[(size_t) i0]) * fr;
            }
            L[i] = L[i] * dry + (v[0] * gBig + v[1] * gSmall) * wet;
            R[i] = R[i] * dry + (v[0] * gSmall + v[1] * gBig) * wet;
            pos = (pos + 1) % len;
        }
    }
    void reset() override { std::fill (buf.begin(), buf.end(), 0.0f); }
};

struct SatFx : Fx
{
    float sr = 44100, pre = 1, wet = 1, dry = 0, out = 1; Rbj lp;
    void prepare (double s, int) override { sr = (float) s; lp.reset(); }
    void setParams (const juce::var& p, double) override
    {
        pre = dbToGain (param (p, "drive", 8)) / 2.5f; lp.set (Rbj::lowpass, sr, param (p, "tone", 9000), 0.707, 0);
        const float mix = param (p, "mix", 100) / 100.0f; wet = mix; dry = 1 - mix; out = dbToGain (param (p, "out", -4));
    }
    void process (float* L, float* R, int n) override
    {
        static const float norm = 1.0f / std::tanh (2.5f);
        for (int i = 0; i < n; ++i)
        {
            const float sl = lp.tick (std::tanh (juce::jlimit (-1.0f, 1.0f, L[i] * pre) * 2.5f) * norm, 0);
            const float sr2 = lp.tick (std::tanh (juce::jlimit (-1.0f, 1.0f, R[i] * pre) * 2.5f) * norm, 1);
            L[i] = (L[i] * dry + sl * wet) * out; R[i] = (R[i] * dry + sr2 * wet) * out;
        }
    }
    void reset() override { lp.reset(); }
};

/** Brick-wall style peak limiter (fast attack, 80 ms release). */
struct LimiterFx : Fx
{
    float sr = 44100, gain = 1, ceil = 0.89f, env = 1, relC = 0.999f;
    void prepare (double s, int) override { sr = (float) s; relC = std::exp (-1.0f / (0.08f * sr)); env = 1; }
    void setParams (const juce::var& p, double) override { gain = dbToGain (param (p, "gain", 0)); ceil = dbToGain (param (p, "ceil", -1)); }
    void process (float* L, float* R, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            const float l = L[i] * gain, r = R[i] * gain, pk = juce::jmax (std::abs (l), std::abs (r));
            const float target = pk > ceil ? ceil / pk : 1.0f;
            env = target < env ? target : target + (env - target) * relC;
            L[i] = juce::jlimit (-ceil, ceil, l * env); R[i] = juce::jlimit (-ceil, ceil, r * env);
        }
    }
    void reset() override { env = 1; }
};

inline std::unique_ptr<Fx> makeBuiltinFx (const juce::String& type)
{
    if (type == "eq")      return std::make_unique<EqFx>();
    if (type == "comp")    return std::make_unique<CompFx>();
    if (type == "reverb")  return std::make_unique<ReverbFx>();
    if (type == "delay")   return std::make_unique<DelayFx>();
    if (type == "chorus")  return std::make_unique<ChorusFx>();
    if (type == "sat")     return std::make_unique<SatFx>();
    if (type == "limiter") return std::make_unique<LimiterFx>();
    return nullptr;
}
} // namespace jaja
