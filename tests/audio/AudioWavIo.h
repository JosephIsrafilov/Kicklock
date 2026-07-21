#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <cmath>
#include <vector>

// =============================================================================
// Thin WAV read/write helpers for the real-audio harness. Writing renders a
// deterministic synthetic fixture out to a real .wav so the harness then loads
// it back through the genuine JUCE WAV codec - the same decode path a DAW would
// use - rather than feeding an in-memory float array that never touched a file.
// Reading decodes any JUCE-readable file to mono float at a target rate, so a
// real recorded override (any rate/depth/channel count) drops in unchanged.
// =============================================================================

namespace AudioWavIo
{
    // Writes a single-channel float signal to a 24-bit WAV at sampleRate.
    // Returns false if the file could not be created.
    inline bool writeMonoWav (const juce::File& file, const std::vector<float>& signal, double sampleRate)
    {
        file.getParentDirectory().createDirectory();
        file.deleteFile();

        juce::WavAudioFormat format;
        std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
        if (stream == nullptr)
            return false;

        std::unique_ptr<juce::AudioFormatWriter> writer (
            format.createWriterFor (stream.get(), sampleRate, 1, 24, {}, 0));
        if (writer == nullptr)
            return false;
        stream.release(); // writer owns the stream now

        juce::AudioBuffer<float> buffer (1, (int) signal.size());
        buffer.clear();
        for (int i = 0; i < (int) signal.size(); ++i)
            buffer.setSample (0, i, std::isfinite (signal[(size_t) i]) ? signal[(size_t) i] : 0.0f);

        return writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    }

    // Decodes any JUCE-readable file to mono float at targetSampleRate. Multi-
    // channel files are summed to mono with equal weight. Linear resample when
    // the source rate differs. Empty vector on failure. (Same behaviour as
    // RealAudioPluginIntegrationTests.cpp's loader, factored out for reuse.)
    inline std::vector<float> loadMonoAtRate (const juce::File& file, double targetSampleRate)
    {
        if (! file.existsAsFile())
            return {};

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        if (reader == nullptr || reader->numChannels < 1 || reader->lengthInSamples < 2)
            return {};

        juce::AudioBuffer<float> source ((int) reader->numChannels, (int) reader->lengthInSamples);
        reader->read (&source, 0, source.getNumSamples(), 0, true, true);

        std::vector<float> mono ((size_t) source.getNumSamples(), 0.0f);
        const float weight = 1.0f / (float) source.getNumChannels();
        for (int ch = 0; ch < source.getNumChannels(); ++ch)
            for (int i = 0; i < source.getNumSamples(); ++i)
                mono[(size_t) i] += source.getSample (ch, i) * weight;

        if (std::abs (reader->sampleRate - targetSampleRate) < 0.5)
            return mono;

        const int outSamples = (int) std::lround ((double) mono.size() * targetSampleRate / reader->sampleRate);
        std::vector<float> resampled ((size_t) std::max (0, outSamples), 0.0f);
        for (int i = 0; i < outSamples; ++i)
        {
            const double srcIndex = (double) i * reader->sampleRate / targetSampleRate;
            const int i0 = (int) std::floor (srcIndex);
            const float frac = (float) (srcIndex - (double) i0);
            const float a = (i0 >= 0 && i0 < (int) mono.size()) ? mono[(size_t) i0] : 0.0f;
            const float b = (i0 + 1 >= 0 && i0 + 1 < (int) mono.size()) ? mono[(size_t) (i0 + 1)] : 0.0f;
            resampled[(size_t) i] = a + frac * (b - a);
        }
        return resampled;
    }
}
