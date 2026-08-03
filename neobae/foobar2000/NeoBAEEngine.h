#pragma once

#include "pch.h"
#include "Configuration.h"
#include "NeoBAEFeatures.h"
#include "BAE_ProbeSongLength.h"

#include <condition_variable>

// Snapshot of prefs taken at decode_initialize so Apply mid-play does not tear state.
struct NeoBAEPlaybackSettings {
	bool loopEnabled = false;
	bool loopInfinite = false;
	unsigned loopCount = 1; // extra plays after first when looping & finite
	bool dlsCompatibilityMode = true; /* default on for new installs */
	bool normalize = false;
	bool useBuiltinBank = true;
	pfc::string8 bankPath;
	unsigned sampleRateHz = (unsigned)kDefaultSampleRate;

	static NeoBAEPlaybackSettings FromConfig();
};

// Thin NeoBAE session used by the foobar2000 input decoder.
//
// libneobae exposes a single process-wide MusicGlobals mixer. Concurrent
// BAEMixer_Open sessions (playback + Waveform Minibar, etc.) corrupt that
// pointer and crash in GM_FreeSong. This class enforces one exclusive mixer
// session at a time for decode. Prepare() also takes the global mutex: probe /
// BAEUtil share process-global X_API state and must not overlap DecodeRun or
// another Prepare (library scan + playlist drop).
class NeoBAEEngine {
public:
	NeoBAEEngine();
	~NeoBAEEngine();

	NeoBAEEngine(const NeoBAEEngine&) = delete;
	NeoBAEEngine& operator=(const NeoBAEEngine&) = delete;

	// Cache file bytes + prefs and resolve duration (+ RMF/ZMF tags) via
	// mixer-free BAE_ProbeSongLengthFromMemory (no MusicGlobals / exclusive lock).
	void Prepare(const void* data, size_t size, const char* pathHint, const NeoBAEPlaybackSettings& settings, abort_callback& abort);

	double GetLengthSeconds() const { return m_lengthSeconds; }
	const char* GetCodecName() const { return m_codecName.get_ptr(); }
	unsigned GetSampleRate() const { return m_settings.sampleRateHz; }
	const BAE_RmfSongMetadata* GetRmfMetadata() const {
		return m_rmfMetadata.present ? &m_rmfMetadata : nullptr;
	}

	// Acquire exclusive mixer, load song, start synthesis.
	void StartDecode(bool allowLooping, abort_callback& abort, const NeoBAEPlaybackSettings* settingsRefresh = nullptr);

	bool DecodeRun(audio_chunk& chunk, abort_callback& abort);
	void Seek(double seconds, abort_callback& abort);
	bool CanSeek() const { return m_started && m_lengthSeconds > 0.0; }

	void Close();

private:
	void WaitExclusive_Locked(std::unique_lock<std::recursive_mutex>& lock, abort_callback& abort);
	bool TryExclusive_Locked();
	void TakeExclusive_Locked();
	void ReleaseExclusive_Locked();

	void EnsureGlobalInit_Locked();
	void ReleaseGlobalRef_Locked();
	void TeardownMixer_Locked(bool clearMetadata, bool releaseGlobalRef = true);
	void OpenMixer_Locked(bool engageAudio);
	void OpenMixerAndSong_Locked(const char* pathHint);
	void SniffCodecFromData_Locked();
	bool IsZmfContainer_Locked() const;
	void SetCodecLabel_Locked(int ftype); // BAEFileType
	void LoadBank_Locked();
	void LoadSong_Locked(const void* data, size_t size, const char* pathHint);
	void ApplyDLSCompat_Locked() const;

	NeoBAEPlaybackSettings m_settings;
	pfc::array_t<t_uint8> m_fileData;
	pfc::string8 m_pathHint;

	void* m_mixer = nullptr; // BAEMixer
	void* m_song = nullptr;  // BAESong

	double m_lengthSeconds = 0.0;
	uint32_t m_lengthMicros = 0;
	pfc::string8 m_codecName;
	BAE_RmfSongMetadata m_rmfMetadata{};

	bool m_started = false;
	bool m_allowLooping = false;
	bool m_holdsGlobalRef = false;
	bool m_holdsExclusive = false;
	unsigned m_loopsDone = 0;
	uint32_t m_lastPosMs = 0;

	pfc::array_t<int16_t> m_pcm;

	static std::recursive_mutex s_mutex;
	static std::condition_variable_any s_cv;
	static NeoBAEEngine* s_sessionOwner;
	static int s_globalRefCount;
};
