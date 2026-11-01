Speech Leveler: Audio Unit Plugin for Final Cut Pro

A custom AU plugin that does not use the built-in Compressor or Limiter from Final Cut Pro.
All processing is handled by custom code in Source/. The signal chain operates in this order:

1. Denoiser (Denoise.h): spectral, with a Wiener filter. It estimates noise automatically from the recording
   (tracks the spectral minimum), so a noise profile/sample is not required. Works best on constant noise:
   room tone, fan noise, microphone hiss, hum.
2. Auto Level (AGC): slowly levels the speech volume to the target level (Target RMS).
   Below the Gate threshold, the gain is frozen. AGC is applied after denoising, so it does not boost noise.
3. Compressor: tames dynamics (Threshold, Ratio, Attack, Release, Makeup).
4. Limiter with 5 ms lookahead: hard ceiling (Ceiling). The output signal never exceeds it,
   even with clicks and sudden peaks.

Default Settings (Variant B)

Parameter	Default	Meaning
Denoise	on	enables the denoiser
Noise Reduction	12 dB	maximum noise attenuation (0-30 dB)
Noise Sensitivity	3 dB	higher means more aggressive (0-12 dB); too much causes "bubbling" and a metallic sound
Auto Level	on	automatic level matching
Target RMS	-20 dB	target speech volume
Gate	-50 dB	below this, AGC does not change the gain
Max Boost	+24 dB	maximum boost for quiet sections
Threshold	-22 dB	compressor threshold
Ratio	3:1	compression ratio
Attack	10 ms	
Release	120 ms	
Makeup	+3 dB	gain boost after compression
Ceiling	-6 dB	limiter hard ceiling

On a test signal, typical peaks land around -9 dB, and the limiter only catches peaks up to -6 dB.
If the ceiling should be -9 dB, set Ceiling = -9 (and optionally Makeup to around 0).

Note: the average (RMS) will end up lower than the peaks by about 12-15 dB, because an average of -6 dB with -9 dB peaks is physically impossible.
For speech, realistically, this is about -24 to -21 dB RMS with -9 dB peaks.

Tuning the Denoiser

⚬ Start with the default 12 dB. If noise is still audible, increase Noise Reduction first, and only then increase Noise Sensitivity.
⚬ If the voice sounds "watery" or metallic, lower the Sensitivity or Reduction.
⚬ On a clean recording without noise, turn off Denoise.

Latency

The plugin reports a latency of approximately 26 ms to the host at 48 kHz (1262 samples: 1023 denoiser + 239 limiter).
It is constant, even when the denoiser is disabled, so that switching it does not shift the audio.

Building on Mac

Requirements: Xcode (or Command Line Tools), CMake (brew install cmake), access to GitHub (CMake will download JUCE 8.0.15 automatically).

cd SpeechLeveler
cmake -B build
cmake --build build --config Release --target SpeechLeveler_AU


Thanks to COPY_PLUGIN_AFTER_BUILD, the Speech Leveler.component file will automatically be placed in
~/Library/Audio/Plug-Ins/Components/. The binary is universal (Apple Silicon + Intel).

Installation and Usage in Final Cut Pro

brew install cmake
cd SpeechLeveler
cmake -B build
cmake --build build --config Release --target SpeechLeveler_AU

1. Close Final Cut Pro and launch it again after building the plugin.
2. Select a clip on the timeline.
3. In the Effects browser, search for Speech Leveler (Audio category, Audio Units section) and drag it onto the clip.
4. You can set the parameters in the plugin window, opened from the Inspector (Audio tab, Modify button next to the effect).
5. Save your final settings as a preset: Effects menu in the Inspector, then Save Effects Preset.