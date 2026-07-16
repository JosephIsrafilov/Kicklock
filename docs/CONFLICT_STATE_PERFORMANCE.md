# Correction-state clustering performance (Layer B)

Layer B state clustering runs on the Dynamic Learn worker while a captured hit
set is finalized. It is not called by the audio callback, Static Analyze path,
or Dynamic runtime selector. The fixed `std::array` map storage therefore adds
no runtime allocation or audio-thread latency.

Category K measures the state-signature clustering loop separately from the
existing rotator/PhaseFix work. The Release/Debug harness uses 1,000 repeats of
a 32-hit batch. The latest Release raw output is `0.12863 ms/32-hit batch at
48 kHz`. This cost is tracked independently; no existing runtime CPU or
latency budget is changed. The per-state `searchCombined` / per-hit refinement
remains part of the already worker-only Dynamic Learn computation.
