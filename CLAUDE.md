# KickLock — Architecture Reference

Interface contracts only. Implementation notes live in code comments, not here.

## Conventions
- All DSP core classes: header-only, templated on `float`, no JUCE plugin-wrapper deps (only `juce_core`/`juce_dsp`).
- Allocation only in `prepare()`. `process*()`/`push*()` never allocate, lock, log, throw.
- `reset()` clears internal state without reallocating (callable from audio thread).

## M1 — DSP core (src/dsp/)

### CorrelationMeter.h
```cpp
class CorrelationMeter
{
public:
    void prepare (double sampleRate, int windowSizeSamples);
    void reset();
    void pushSample (float a, float b);       // O(1) rolling update
    float getCorrelation() const;              // smoothed, 0..100 (0=inverted,100=aligned)
    void setSmoothingTimeMs (float ms);        // EMA time constant, settable pre/post prepare
};
```
Rolling window via running sums (sumA, sumB, sumAB, sumA2, sumB2) over a circular buffer of the last `windowSizeSamples` values, add-new/subtract-old, O(1) per sample. Raw Pearson r in [-1,1] mapped to [0,100] via `(r+1)*50`, then EMA-smoothed. Buffer allocated in `prepare`.

### FractionalDelayLine.h
```cpp
enum class InterpolationType { Linear, Allpass };

class FractionalDelayLine
{
public:
    void prepare (double sampleRate, float maxDelayMs);   // allocates for max delay
    void reset();
    void setDelaySamples (float delaySamples);            // clamped [0, maxDelaySamples]
    void setInterpolationType (InterpolationType);
    float processSample (float input);
};
```
Unidirectional delay, range [0, maxDelayMs] (caller/processor picks which channel gets fed based on lead/lag sign — no sign handling inside this class). Circular buffer sized `ceil(maxDelayMs/1000*sampleRate)+1`, allocated in `prepare`. Allpass mode uses a single first-order allpass interpolator with running state; switching type calls `reset()` internally on the interpolator state only.

### AllpassPhaseRotator.h
```cpp
class AllpassPhaseRotator
{
public:
    void prepare (double sampleRate, int numStages);   // numStages clamped [2,4]
    void reset();
    void setParameters (float frequencyHz, float q);   // same freq/Q applied to all stages
    float processSample (float input);
};
```
`std::array<juce::dsp::IIR::Filter<float>, 4> stages` (fixed size, unused stages bypassed via active count, not resized). Coefficients via `juce::dsp::IIR::Coefficients<float>::makeAllPass`. Coefficient objects created in `prepare`/`setParameters` (both audio-thread-illegal call sites already — `setParameters` is called from `parameterChanged` callback context in M2, not per-sample).

### tests/DspTests.cpp
JUCE `UnitTest`-based, console target, no audio I/O. Cases: identical-signal correlation ~100, inverted-signal correlation ~0, delay-compensation correctness (known fractional shift recovers correlation after delay applied to lagging signal).

## M2 — PluginProcessor (src/PluginProcessor.h/.cpp, src/util/ScopeFifo.h)

### util/ScopeFifo.h
```cpp
class ScopeFifo
{
public:
    void prepare (int capacitySamples);   // allocates, call from prepareToPlay
    void reset();
    void pushSample (float mainValue, float sidechainValue);  // audio thread, lock-free, drops sample on overflow
    int readAvailable (float* mainOut, float* sidechainOut, int maxSamples);  // message/UI thread, lock-free pop
};
```
Backed by `juce::AbstractFifo` + two parallel `std::vector<float>` (main, sidechain) sized at `prepare`. `pushSample`/`readAvailable` never allocate.

### PluginProcessor parameters (APVTS)
IDs (use `juce::ParameterID{id, 1}` — JUCE 8 versioned IDs):
- `"delayMs"` — AudioParameterFloat, range [-50, 50], default 0, step 0.01. Sign convention: positive delays the **main** (bass) bus, negative delays the **sidechain** (kick) bus by `abs(value)` ms.
- `"delayInterp"` — AudioParameterChoice, `{"Linear","Allpass"}`, default "Linear".
- `"polarityInvert"` — AudioParameterBool, default false. Inverts the main bus.
- `"rotatorFreq"` — AudioParameterFloat, range [20, 2000] Hz, skewed (`NormalisableRange<float>(20.f, 2000.f, 0.f, 0.3f)`), default 200.
- `"rotatorQ"` — AudioParameterFloat, range [0.1, 10], default 0.7.
- `"rotatorStages"` — AudioParameterChoice, `{"2","3","4"}`, default "2".

Cache raw parameter pointers (`apvts.getRawParameterValue(id)`) in the constructor — no tree lookups inside `processBlock`.

### DSP member layout
Per-channel (stereo, up to 2 channels; bus may be mono — use channel 0 only in that case):
- `std::array<FractionalDelayLine, 2> mainDelay, sidechainDelay;`
- `std::array<AllpassPhaseRotator, 2> rotator;` (applied to main bus only, post-delay, post-polarity)
- Single `CorrelationMeter correlationMeter;` — fed mono-summed (average of active channels) post-processing samples from both buses.
- Single `ScopeFifo scopeFifo;` — fed the same mono-summed pair, capacity 8192 samples.
- `std::atomic<float> correlationPercent {0.0f};` updated each block from `correlationMeter.getCorrelation()`, read by editor in M3.

### processBlock order
1. `ScopedNoDenormals`. Get main bus buffer (bus 0) and sidechain bus buffer (bus 1) via `getBusBuffer`. If sidechain bus is disabled/has 0 channels, skip steps 3-5 involving it (fall through main-only, correlation meter not pushed, scope not pushed) — do not crash or branch-allocate.
2. If `polarityInvert` param true, negate main bus buffer in place (all channels).
3. Read `delayMs`. If `> 0`, apply to `mainDelay[ch]` (`setDelaySamples(delayMs/1000*sampleRate)`), `sidechainDelay[ch]` set to 0. If `< 0`, apply `abs` to `sidechainDelay[ch]`, `mainDelay[ch]` set to 0. Apply `setInterpolationType` from `delayInterp` param **only when the choice value changed since the previous block** (cache last index; calling `setInterpolationType` unconditionally resets allpass interpolator state every block and audibly breaks the filter — this is a hard constraint, not a style choice).
4. Run `mainDelay`/`sidechainDelay` `processSample` per-channel per-sample on the respective bus buffers.
5. Run `rotator[ch].setParameters(rotatorFreq, rotatorQ)` every block (cheap, no realloc) using current param values. If `rotatorStages` choice changed since previous block, call `rotator[ch].prepare(sampleRate, stages)` (cache last stage count — `prepare` resets filter state, so only call on actual change, not every block). Then run `rotator[ch].processSample` per-channel per-sample on the main bus buffer.
6. Per-sample, compute mono-summed main/sidechain values (post steps 2-5), `correlationMeter.pushSample(mainMono, sidechainMono)`, `scopeFifo.pushSample(mainMono, sidechainMono)`.
7. After the block, `correlationPercent.store (correlationMeter.getCorrelation())`.

### State
`getStateInformation`/`setStateInformation` via `apvts.copyState()` → `juce::MemoryOutputStream` (XML), and `juce::ValueTree::fromXml` → `apvts.replaceState()`.

`prepareToPlay` allocates/prepares all DSP members and `scopeFifo` (capacity 8192). No allocation anywhere else.

## M3 — PluginEditor (src/ui/, src/PluginEditor.h/.cpp)

UI thread only. No audio-thread code here. Timer-driven polling of `scopeFifo`/`correlationPercent`, not push-based.

### ui/LookAndFeel.h
```cpp
class KickLockLookAndFeel : public juce::LookAndFeel_V4
{
public:
    KickLockLookAndFeel();   // sets ColourScheme + a few Slider/ComboBox colour ids in ctor body
};
```
No overridden draw methods — palette only via `setColour`. Background near-black, accent teal/orange, no allocation beyond ctor.

### ui/Oscilloscope.h/.cpp
```cpp
class Oscilloscope : public juce::Component, private juce::Timer
{
public:
    explicit Oscilloscope (ScopeFifo& fifoToRead);   // capacity fixed, no realloc after ctor
    ~Oscilloscope() override;
    void paint (juce::Graphics&) override;
    void resized() override;
private:
    void timerCallback() override;   // ~30Hz: readAvailable into ring buffers, repaint()
};
```
History length fixed at construction (2048 samples per trace, `std::vector<float>` sized once in ctor — this is UI-thread allocation, happens once, not per-frame). `timerCallback` calls `fifo.readAvailable` into a scratch stack buffer (fixed-size `std::array<float, 256>`, one read per tick, not allocated), shifts new samples into the two ring buffers, calls `repaint()`. `paint()` draws two `juce::Path` traces (main = accent colour, sidechain = second colour) scaled to component bounds, y-centered at half height.

### ui/CorrelationDisplay.h
```cpp
class CorrelationDisplay : public juce::Component, private juce::Timer
{
public:
    explicit CorrelationDisplay (std::atomic<float>& percentToRead);
    void paint (juce::Graphics&) override;
private:
    void timerCallback() override;   // ~15Hz: repaint() only, no allocation
};
```
`paint()` draws the cached last-read percent as large centered text ("87%"), colour interpolated red (0%) -> green (100%) via `juce::Colour::interpolatedWith`.

### PluginEditor.h/.cpp
```cpp
class KickLockAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit KickLockAudioProcessorEditor (KickLockAudioProcessor&);
    ~KickLockAudioProcessorEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;
};
```
Fixed size 700x450 (was 500x300 in M0 — resize now that there's real content). Owns: `KickLockLookAndFeel`, one `Oscilloscope` (top ~60% of window), one `CorrelationDisplay` (top-right corner overlay or adjacent to scope), and controls for all 6 APVTS params:
- `juce::Slider delayMsSlider` (linear, horizontal, with attached `juce::Label`) + `SliderAttachment`
- `juce::ComboBox delayInterpBox` (populated "Linear"/"Allpass" — must match `AudioParameterChoice` order) + `ComboBoxAttachment`
- `juce::ToggleButton polarityInvertButton` ("Invert Polarity") + `ButtonAttachment`
- `juce::Slider rotatorFreqSlider` (skewed to match param range, `juce::Slider::TextBoxBelow`) + `SliderAttachment`
- `juce::Slider rotatorQSlider` + `SliderAttachment`
- `juce::ComboBox rotatorStagesBox` (populated "2"/"3"/"4") + `ComboBoxAttachment`

All attachments are `std::unique_ptr<...Attachment>` members, constructed in the ctor body after the controls are added as children (`addAndMakeVisible`), referencing `audioProcessor.apvts`. `resized()` lays out controls in a simple grid/flexbox under the scope — exact pixel layout is implementation's call, no pixel-perfect spec, just: no overlapping components, all controls + labels visible without scrolling within the window (now 720x560).

## M4 — Analyze (auto-align) + scope zoom

### util/RawCaptureBuffer.h
```cpp
class RawCaptureBuffer
{
public:
    void prepare (int capacitySamples);   // allocates, call from prepareToPlay
    void reset();
    void push (float bassValue, float kickValue) noexcept;   // audio thread, lock-free ring write
    int  snapshot (std::vector<float>& bassOut, std::vector<float>& kickOut) const; // UI thread, chronological copy
};
```
Rolling capture of **raw pre-processing** mono bass/kick (capacity ~2s, sized in `prepareToPlay`). Single atomic `writePos`; audio thread writes one pair per sample with no allocation/lock. `snapshot` (message thread) copies the whole ring oldest-first. A few torn samples at the wrap boundary are acceptable for a correlation estimate over tens of thousands of samples — no double buffering. Fed at the very top of `processBlock` (before polarity/delay/rotator), mono-summed the same way the meter/scope feed is, skipped when there's no sidechain.

### dsp/AlignmentAnalyzer.h
```cpp
struct AlignmentResult { bool valid; float delayMs; bool invertPolarity; float beforeMatch; float afterMatch; };

class AlignmentAnalyzer
{
public:
    static AlignmentResult analyze (const float* bass, const float* kick, int numSamples,
                                    double sampleRate, float maxDelayMs,
                                    float lowHz = 30.0f, float highHz = 120.0f, int maxWindow = 32768);
};
```
Pure/header-only (std only, no JUCE) so the unit tests include it directly.

**Band-limiting (the fix for random-feeling results).** Kick and bass only interact in the sub/low range; their broadband waveforms barely resemble each other, so a full-range cross-correlation locks onto whatever loud transient (e.g. the kick click) happens to line up in a given capture — jumpy, arbitrary lags. Both signals are therefore band-passed to `[lowHz, highHz]` (default 30–120 Hz) with the **same** zero-phase filter before correlating. Identical filtering imposes identical group delay on both, so the measured relative lag is preserved while the out-of-band noise is removed. Filter is an RBJ band-pass biquad applied forward-then-reverse (filtfilt-style) for zero phase; header-local `Biquad` struct, no JUCE.

Then time-domain cross-correlation `c[D] = sum_n a[n]*b[n+D]` over the band-limited most-recent `min(numSamples, maxWindow)` samples, `D` in `[-maxLag, maxLag]` where `maxLag = maxDelayMs/1000*sr`. Normalised by `sqrt(energyBass*energyKick)` of the **band-limited** signals; `norm < 1e-6` → `valid=false` (low-end silence gate). Best `|c|` gives the integer lag, then parabolic interpolation over the three correlation samples around the peak refines it to sub-sample; `delayMs = D*/sr*1000` clamped to `±maxDelayMs`. **Sign convention matches the processor**: `D*>0` (kick lags) → delay the bass (positive `delayMs`); `D*<0` (kick leads) → delay the kick (negative). `invertPolarity = (best c < 0)`. `beforeMatch`/`afterMatch` map r→[0,100] via `(r+1)*50` (same as `CorrelationMeter`) at zero offset vs. after the recommended alignment.

### PluginProcessor::analyzeAndApply()
Message thread only. Snapshots `rawCapture`, runs `AlignmentAnalyzer::analyze`, and on `valid` **auto-applies** the result: writes `delayMs` and `polarityInvert` via `getParameter(id)->setValueNotifyingHost(convertTo0to1(...))` so both the sliders and the host update. Returns the `AlignmentResult` for the editor to report. The rotator is intentionally not touched (no single mathematical best).

### Oscilloscope zoom
`setTimeZoom(1..16)` / `setAmpZoom(1..8)` + getters. Time zoom draws only the most-recent `historyLength/zoom` samples stretched across the width (inspect alignment sample-by-sample); amp zoom multiplies the smoothed auto-gain. `mouseWheelMove` nudges time zoom (Shift = amp) and fires `std::function<void()> onZoomChanged` so the editor's sliders stay in sync. A read-out shows the visible window in ms (`1000/timeZoom`) and the amp factor.

### Editor additions
`analyzeButton` (orange) calls `analyzeAndApply` and writes a one-line result into `analyzeResultLabel` ("Delay +3.2 ms · Polarity flipped · match 41% -> 88%", or a "needs kick + bass" hint when invalid). `timeZoomSlider`/`ampZoomSlider` drive the scope; `onZoomChanged` writes wheel-zoom back into them (`dontSendNotification` to avoid feedback loops).

### tests/DspTests.cpp (M4 additions)
`AlignmentAnalyzer` cases: recovers a known 40-sample lag with correct negative sign; **ignores out-of-band interference** — a loud shared 5 kHz click at lag 0 plus a low-end thump at lag 40 must recover the 40-sample low-end lag, not the click's zero offset (the regression guard for the band-limiting fix); detects an inverted-but-aligned pair (polarity flip, ~0 delay, before <5% / after >95%); silent input reported `valid=false`.
