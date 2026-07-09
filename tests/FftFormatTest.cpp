#include <iostream>
#include <vector>
#include <juce_dsp/juce_dsp.h>

int main()
{
    juce::dsp::FFT fft (3); // size = 8
    std::vector<float> data (16, 0.0f); // 2 * size
    
    // Create a simple DC signal
    for (int i = 0; i < 8; ++i)
        data[i] = 1.0f;
        
    fft.performFrequencyOnlyForwardTransform (data.data());
    
    for (int i = 0; i < 16; ++i)
        std::cout << "data[" << i << "] = " << data[i] << "\n";
        
    return 0;
}
