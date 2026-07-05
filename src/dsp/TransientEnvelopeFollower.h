#pragma once

#include <JuceHeader.h>
#include <algorithm>
#include <cmath>

class TransientEnvelopeFollower
{
public:
    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        fast.prepare ({ sampleRate, 1, 1 });
        slow.prepare ({ sampleRate, 1, 1 });
        fast.setLevelCalculationType (juce::dsp::BallisticsFilterLevelCalculationType::peak);
        slow.setLevelCalculationType (juce::dsp::BallisticsFilterLevelCalculationType::peak);
        fast.setAttackTime (0.2f);
        fast.setReleaseTime (12.0f);
        slow.setAttackTime (12.0f);
        slow.setReleaseTime (140.0f);
        setWindow (attackMs, holdMs, releaseMs);
        reset();
    }

    void reset()
    {
        fast.reset();
        slow.reset();
        phase = Phase::Idle;
        phaseSample = 0;
        armed = true;
        current = 0.0f;
    }

    void setWindow (float atkMs, float hldMs, float relMs) noexcept
    {
        attackMs = juce::jlimit (0.1f, 50.0f, atkMs);
        holdMs = juce::jlimit (0.0f, 250.0f, hldMs);
        releaseMs = juce::jlimit (1.0f, 500.0f, relMs);
        attackSamples = msToSamples (attackMs);
        holdSamples = msToSamples (holdMs);
        releaseSamples = msToSamples (releaseMs);
    }

    void setTriggerRatio (float r) noexcept
    {
        triggerRatio = juce::jlimit (1.05f, 20.0f, r);
    }

    float processSample (float kick) noexcept
    {
        const float x = std::abs (kick);
        const float fastEnv = fast.processSample (0, x);
        const float slowEnv = slow.processSample (0, x);

        if (! armed && fastEnv <= slowEnv * rearmRatio)
            armed = true;

        if (armed && phase == Phase::Idle && fastEnv > juce::jmax (1.0e-6f, slowEnv * triggerRatio))
        {
            armed = false;
            phase = Phase::Attack;
            phaseSample = 0;
        }

        switch (phase)
        {
            case Phase::Idle:
                current = 0.0f;
                break;

            case Phase::Attack:
                current = attackSamples <= 1 ? 1.0f : (float) phaseSample / (float) attackSamples;
                if (++phaseSample >= attackSamples)
                {
                    phase = holdSamples > 0 ? Phase::Hold : Phase::Release;
                    phaseSample = 0;
                    current = 1.0f;
                }
                break;

            case Phase::Hold:
                current = 1.0f;
                if (++phaseSample >= holdSamples)
                {
                    phase = Phase::Release;
                    phaseSample = 0;
                }
                break;

            case Phase::Release:
                current = releaseSamples <= 1 ? 0.0f : 1.0f - (float) phaseSample / (float) releaseSamples;
                if (++phaseSample >= releaseSamples)
                {
                    phase = Phase::Idle;
                    phaseSample = 0;
                    current = 0.0f;
                }
                break;
        }

        return juce::jlimit (0.0f, 1.0f, current);
    }

private:
    enum class Phase { Idle, Attack, Hold, Release };

    int msToSamples (float ms) const noexcept
    {
        return juce::jmax (1, (int) std::lround (sampleRate * (double) ms / 1000.0));
    }

    double sampleRate = 44100.0;
    float attackMs = 2.0f;
    float holdMs = 18.0f;
    float releaseMs = 80.0f;
    float triggerRatio = 3.0f;
    float rearmRatio = 1.15f;
    int attackSamples = 1;
    int holdSamples = 1;
    int releaseSamples = 1;
    int phaseSample = 0;
    float current = 0.0f;
    bool armed = true;
    Phase phase = Phase::Idle;
    juce::dsp::BallisticsFilter<float> fast;
    juce::dsp::BallisticsFilter<float> slow;
};
