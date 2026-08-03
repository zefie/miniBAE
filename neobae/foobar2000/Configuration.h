#pragma once

#include "pch.h"

// Preferences page GUID (also returned by InputDecoder::g_get_preferences_guid).
static constexpr GUID PreferencesPageGUID =
{ 0x766f4c38, 0xdb94, 0x4f5f, { 0x9c, 0x4d, 0x2c, 0xdb, 0x9f, 0x9c, 0xfa, 0x28 } };

// Decoder service GUID.
static constexpr GUID InputDecoderGUID =
{ 0xecae3dd0, 0x3610, 0x410a, { 0xa9, 0x7a, 0xae, 0x12, 0xd8, 0x13, 0x12, 0xa5 } };

namespace cfg {
	// Loop playback past the reported (non-looped) duration.
	extern cfg_var_modern::cfg_bool LoopEnabled;
	// Extra plays after the first (1 = hear song twice). Ignored when LoopInfinite is set.
	extern cfg_var_modern::cfg_int LoopCount;
	// Engine max / "infinity" loop mode (playbae uses 30000).
	extern cfg_var_modern::cfg_bool LoopInfinite;
	// Absolute path to HSB/ZSB/SF2/DLS bank. Empty = built-in bank when available.
	extern cfg_var_modern::cfg_string BankPath;
	// Prefer built-in patches even if BankPath is set.
	extern cfg_var_modern::cfg_bool UseBuiltinBank;
	// Broader DLS support (disables MobileBAE quirks). playbae -dlscompat.
	extern cfg_var_modern::cfg_bool DLSCompatibilityMode;
	// MIDI peak estimate normalize (~ -1 dBFS).
	extern cfg_var_modern::cfg_bool Normalize;
	// NeoBAE mixer sample rate in Hz (engine timing/timbre depend on this).
	extern cfg_var_modern::cfg_int SampleRate;
}

// Defaults
static constexpr bool kDefaultLoopEnabled = false;
static constexpr int64_t kDefaultLoopCount = 1;
static constexpr bool kDefaultLoopInfinite = false;
static constexpr bool kDefaultUseBuiltinBank = true;
static constexpr bool kDefaultDLSCompatibilityMode = true; /* default on for new installs */
static constexpr bool kDefaultNormalize = false;
static constexpr int64_t kDefaultSampleRate = 44100;

// Matches playbae's "effectively infinite" SetLoops value.
static constexpr int16_t kNeoBAEInfiniteLoops = 30000;

// Rates supported by BAERate (positive enum values). Foobar resamples to the
// output device rate — we report the synth rate on chunks, like foo_midi.
static constexpr unsigned kNeoBAESampleRates[] = {
	8000, 11025, 16000, 22050, 24000, 32000, 40000, 44100, 48000
};
static constexpr size_t kNeoBAESampleRateCount = sizeof(kNeoBAESampleRates) / sizeof(kNeoBAESampleRates[0]);

inline unsigned NormalizeSampleRateHz(int64_t hz)
{
	if (hz <= 0)
		return (unsigned)kDefaultSampleRate;

	unsigned best = (unsigned)kDefaultSampleRate;
	int64_t bestDiff = INT64_MAX;
	for (size_t i = 0; i < kNeoBAESampleRateCount; ++i) {
		const unsigned r = kNeoBAESampleRates[i];
		const int64_t d = (hz > (int64_t)r) ? (hz - (int64_t)r) : ((int64_t)r - hz);
		if (d < bestDiff) {
			bestDiff = d;
			best = r;
		}
	}
	return best;
}

static constexpr unsigned kChannels = 2;
static constexpr unsigned kBitsPerSample = 16;
