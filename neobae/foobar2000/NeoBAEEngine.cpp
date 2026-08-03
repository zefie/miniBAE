#include "pch.h"
#include "NeoBAEEngine.h"

#include "NeoBAE.h"
#include "BAE_API.h"
#if USE_NATIVE_DLS == TRUE
#include "GenDLS_MobileBAE.h"
#endif

#if USE_SF2_SUPPORT == TRUE && _USING_FLUIDLITE == TRUE
extern "C" {
	void GM_SetMixerSF2Mode(bool isSF2);
	OPErr GM_LoadSF2Soundfont(const char* sf2_path);
	void GM_UnloadSF2Soundfont(void);
}
#endif

std::recursive_mutex NeoBAEEngine::s_mutex;
std::condition_variable_any NeoBAEEngine::s_cv;
NeoBAEEngine* NeoBAEEngine::s_sessionOwner = nullptr;
int NeoBAEEngine::s_globalRefCount = 0;

NeoBAEPlaybackSettings NeoBAEPlaybackSettings::FromConfig()
{
	NeoBAEPlaybackSettings s;
	s.loopEnabled = cfg::LoopEnabled.get();
	s.loopInfinite = cfg::LoopInfinite.get();
	const int64_t count = cfg::LoopCount.get();
	s.loopCount = (count < 1) ? 1u : (count > 30000 ? 30000u : (unsigned)count);
	s.dlsCompatibilityMode = cfg::DLSCompatibilityMode.get();
	s.normalize = cfg::Normalize.get();
	s.useBuiltinBank = cfg::UseBuiltinBank.get();
	s.bankPath = cfg::BankPath.get();
	s.sampleRateHz = NormalizeSampleRateHz(cfg::SampleRate.get());
	return s;
}

static void ThrowBAE(const char* what, BAEResult err)
{
	pfc::string8 msg;
	msg << "NeoBAE: " << what;
	if (err != BAE_NO_ERROR)
		msg << " (error " << (int)err << ")";
	throw exception_io_data(msg.get_ptr());
}

NeoBAEEngine::NeoBAEEngine() = default;

NeoBAEEngine::~NeoBAEEngine()
{
	Close();
}

void NeoBAEEngine::WaitExclusive_Locked(std::unique_lock<std::recursive_mutex>& lock, abort_callback& abort)
{
	while (s_sessionOwner != nullptr && s_sessionOwner != this) {
		abort.check();
		s_cv.wait_for(lock, std::chrono::milliseconds(50));
	}
}

bool NeoBAEEngine::TryExclusive_Locked()
{
	if (s_sessionOwner != nullptr && s_sessionOwner != this)
		return false;
	TakeExclusive_Locked();
	return true;
}

void NeoBAEEngine::TakeExclusive_Locked()
{
	s_sessionOwner = this;
	m_holdsExclusive = true;
}

void NeoBAEEngine::ReleaseExclusive_Locked()
{
	if (!m_holdsExclusive)
		return;
	if (s_sessionOwner == this)
		s_sessionOwner = nullptr;
	m_holdsExclusive = false;
	s_cv.notify_all();
}

void NeoBAEEngine::EnsureGlobalInit_Locked()
{
	if (m_holdsGlobalRef)
		return;

	if (s_globalRefCount == 0) {
		if (BAE_Setup() != 0)
			throw exception_io_data("NeoBAE: BAE_Setup failed");
	}
	++s_globalRefCount;
	m_holdsGlobalRef = true;
}

void NeoBAEEngine::ReleaseGlobalRef_Locked()
{
	if (!m_holdsGlobalRef)
		return;

	if (s_globalRefCount > 0) {
		--s_globalRefCount;
		if (s_globalRefCount == 0)
			BAE_Cleanup();
	}
	m_holdsGlobalRef = false;
}

void NeoBAEEngine::TeardownMixer_Locked(bool clearMetadata, bool releaseGlobalRef)
{
	BAESong song = (BAESong)m_song;
	BAEMixer mixer = (BAEMixer)m_mixer;

	if (song) {
		BAESong_Stop(song, FALSE);
		BAESong_Delete(song);
		m_song = nullptr;
	}

	if (mixer) {
		BAEMixer_SetSongNormalizeGain(mixer, 100);
#if USE_SF2_SUPPORT == TRUE && _USING_FLUIDLITE == TRUE
		GM_UnloadSF2Soundfont();
		GM_SetMixerSF2Mode(FALSE);
#endif
#if USE_NATIVE_DLS == TRUE
		BAEMixer_UnloadXMFDLSOverlayBank(mixer);
		BAEMixer_UnloadDLSBank(mixer);
		GM_SetMixerDLSMode(FALSE);
#endif
		BAEMixer_UnloadBanks(mixer);
		BAE_ReleaseAudioCard(nullptr);
		BAEMixer_Delete(mixer);
		m_mixer = nullptr;
	}

	m_started = false;
	m_allowLooping = false;
	m_loopsDone = 0;
	m_lastPosMs = 0;

	if (clearMetadata) {
		m_lengthSeconds = 0.0;
		m_lengthMicros = 0;
		m_codecName = "";
		memset(&m_rmfMetadata, 0, sizeof(m_rmfMetadata));
	}

	if (releaseGlobalRef)
		ReleaseGlobalRef_Locked();
}

void NeoBAEEngine::Close()
{
	std::lock_guard<std::recursive_mutex> lock(s_mutex);
	TeardownMixer_Locked(true);
	ReleaseExclusive_Locked();
	m_fileData.set_size(0);
	m_pathHint = "";
}

void NeoBAEEngine::ApplyDLSCompat_Locked() const
{
#if USE_NATIVE_DLS == TRUE
	GM_DLS_SetMobileBAEQuirks(m_settings.dlsCompatibilityMode ? false : true);
#endif
}

bool NeoBAEEngine::IsZmfContainer_Locked() const
{
	// ZMF is the ZREZ resource-map variant of RMF (same BAEFileType).
	if (m_fileData.get_size() >= 4) {
		const unsigned char* b = m_fileData.get_ptr();
		if (b[0] == 'Z' && b[1] == 'R' && b[2] == 'E' && b[3] == 'Z')
			return true;
	}
	if (m_pathHint.get_length()) {
		const char* ext = strrchr(m_pathHint.get_ptr(), '.');
		if (ext && _stricmp(ext, ".zmf") == 0)
			return true;
	}
	return false;
}

void NeoBAEEngine::SetCodecLabel_Locked(int ftype)
{
	if (IsZmfContainer_Locked()) {
		m_codecName = "ZMF";
		return;
	}
	const char* typeName = X_GetFileTypeString((BAEFileType)ftype);
	if (typeName && typeName[0] && ftype != (int)BAE_INVALID_TYPE)
		m_codecName = typeName;
	else if (m_pathHint.get_length()) {
		const char* ext = strrchr(m_pathHint.get_ptr(), '.');
		m_codecName = (ext && ext[1]) ? (ext + 1) : "MIDI";
	} else {
		m_codecName = "MIDI";
	}
}

void NeoBAEEngine::SniffCodecFromData_Locked()
{
	BAEFileType ftype = BAE_INVALID_TYPE;
	const t_size size = m_fileData.get_size();
	const unsigned char* bytes = m_fileData.get_ptr();

	if (size >= 4) {
		if ((bytes[0] == 'Z' && bytes[1] == 'R' && bytes[2] == 'E' && bytes[3] == 'Z') ||
		    (bytes[0] == 'I' && bytes[1] == 'R' && bytes[2] == 'E' && bytes[3] == 'Z'))
			ftype = BAE_RMF;
	}
	if (ftype == BAE_INVALID_TYPE && size > 0) {
		int32_t probe = (int32_t)((size > 64) ? 64 : size);
		ftype = X_DetermineFileTypeByData(bytes, probe);
	}
	SetCodecLabel_Locked(ftype);
}

void NeoBAEEngine::LoadBank_Locked()
{
	BAEMixer mixer = (BAEMixer)m_mixer;
	const char* path = m_settings.bankPath.get_ptr();
	const bool havePath = path && path[0];

	BAEMixer_UnloadBanks(mixer);
#if USE_NATIVE_DLS == TRUE
	GM_SetMixerDLSMode(FALSE);
	BAEMixer_UnloadXMFDLSOverlayBank(mixer);
	BAEMixer_UnloadDLSBank(mixer);
#endif
#if USE_SF2_SUPPORT == TRUE && _USING_FLUIDLITE == TRUE
	GM_UnloadSF2Soundfont();
	GM_SetMixerSF2Mode(FALSE);
#endif

	if (!havePath || m_settings.useBuiltinBank) {
#if _BUILT_IN_PATCHES == TRUE
		BAEBankToken tok = 0;
		const BAEResult err = BAEMixer_LoadBuiltinBank(mixer, &tok);
		if (err != BAE_NO_ERROR)
			ThrowBAE("failed to load built-in bank", err);
		return;
#else
		ThrowBAE("no bank configured and built-in patches are unavailable", BAE_GENERAL_ERR);
#endif
	}

	const char* ext = strrchr(path, '.');
#if USE_SF2_SUPPORT == TRUE && _USING_FLUIDLITE == TRUE
	if (ext && (_stricmp(ext, ".sf2") == 0
#if SF3_SUPPORT > 0
		|| _stricmp(ext, ".sf3") == 0 || _stricmp(ext, ".sfo") == 0
#endif
		)) {
#if _BUILT_IN_PATCHES == TRUE && _LOAD_BUILTIN_PATCHES_FOR_SF2 == TRUE
		BAEBankToken bt = 0;
		if (BAEMixer_LoadBuiltinBank(mixer, &bt) == BAE_NO_ERROR && bt)
			BAEMixer_SendBankToBack(mixer, bt);
#endif
		if (GM_LoadSF2Soundfont(path) != NO_ERR)
			ThrowBAE("SF2 bank load failed", BAE_BAD_FILE);
		GM_SetMixerSF2Mode(TRUE);
		return;
	}
#endif

#if USE_NATIVE_DLS == TRUE
	if (ext && _stricmp(ext, ".dls") == 0) {
#if _BUILT_IN_PATCHES == TRUE && _LOAD_BUILTIN_PATCHES_FOR_DLS == TRUE
		BAEBankToken builtin = 0;
		if (BAEMixer_LoadBuiltinBank(mixer, &builtin) == BAE_NO_ERROR && builtin)
			BAEMixer_SendBankToBack(mixer, builtin);
#endif
		const BAEResult dls = BAEMixer_LoadDLSBank(mixer, (BAEPathName)path);
		if (dls != BAE_NO_ERROR)
			ThrowBAE("DLS bank load failed", dls);
		GM_SetMixerDLSMode(TRUE);
		return;
	}
#endif

	BAEBankToken tok = 0;
	const BAEResult err = BAEMixer_AddBankFromFile(mixer, (BAEPathName)path, &tok);
	if (err != BAE_NO_ERROR)
		ThrowBAE("bank load failed", err);
}

void NeoBAEEngine::LoadSong_Locked(const void* data, size_t size, const char* /*pathHint*/)
{
	BAEMixer mixer = (BAEMixer)m_mixer;

	BAELoadResult loaded{};
	const BAEResult err = BAEMixer_LoadFromMemory(mixer, data, (uint32_t)size, &loaded);
	if (err != BAE_NO_ERROR || loaded.type != BAE_LOAD_TYPE_SONG || !loaded.data.song) {
		BAELoadResult_Cleanup(&loaded);
		ThrowBAE("failed to load song (unsupported or invalid format)", err != BAE_NO_ERROR ? err : BAE_BAD_FILE);
	}

	const BAEFileType fileType = loaded.fileType;
	BAESong song = loaded.data.song;
	loaded.data.song = nullptr;
	loaded.type = BAE_LOAD_TYPE_NONE;

	SetCodecLabel_Locked((int)fileType);
	m_song = song;

	uint32_t len = 0;
	if (BAESong_GetMicrosecondLength(song, &len) != BAE_NO_ERROR || len == 0)
		len = 1000;
	m_lengthMicros = len;
	m_lengthSeconds = (double)len / 1000000.0;
}

void NeoBAEEngine::OpenMixer_Locked(bool engageAudio)
{
	EnsureGlobalInit_Locked();

	BAEMixer mixer = BAEMixer_New();
	if (!mixer)
		ThrowBAE("BAEMixer_New failed", BAE_MEMORY_ERR);
	m_mixer = mixer;

	const unsigned hz = NormalizeSampleRateHz((int64_t)m_settings.sampleRateHz);
	m_settings.sampleRateHz = hz;

	// BAERate enum values are Hz for the standard rates we expose.
	BAEResult err = BAEMixer_Open(
		mixer,
		(BAERate)hz,
		BAE_LINEAR_INTERPOLATION,
		(BAEAudioModifiers)(BAE_USE_16 | BAE_USE_STEREO),
		64, 8, 64,
		engageAudio ? TRUE : FALSE);
	if (err != BAE_NO_ERROR)
		ThrowBAE("BAEMixer_Open failed", err);

	if (engageAudio) {
		if (BAE_AcquireAudioCard(nullptr, hz, kChannels, kBitsPerSample) != 0)
			ThrowBAE("BAE_AcquireAudioCard failed", BAE_GENERAL_ERR);
	}
}

void NeoBAEEngine::OpenMixerAndSong_Locked(const char* pathHint)
{
	OpenMixer_Locked(true);
	ApplyDLSCompat_Locked();
	LoadBank_Locked();
	LoadSong_Locked(m_fileData.get_ptr(), m_fileData.get_size(), pathHint);
}

static bool IsRmfOrZmfBytes(const void* data, size_t size)
{
	if (!data || size < 4)
		return false;
	const auto* b = static_cast<const unsigned char*>(data);
	return (b[0] == 'I' && b[1] == 'R' && b[2] == 'E' && b[3] == 'Z')
		|| (b[0] == 'Z' && b[1] == 'R' && b[2] == 'E' && b[3] == 'Z');
}

static void CopyRmfField(char* dst, size_t dstBytes, void* data, uint32_t size, BAEInfoType type)
{
	if (!dst || dstBytes == 0)
		return;
	dst[0] = 0;
	(void)BAEUtil_GetRmfSongInfo(data, size, 0, type, dst, (uint32_t)dstBytes);
}

/* Mixer-free RMF/ZMF tags via public BAEUtil (does not need the probe DLL entry). */
static void FillRmfMetadataFromUtil(BAE_RmfSongMetadata& out, void* data, uint32_t size)
{
	memset(&out, 0, sizeof(out));
	if (!data || size == 0 || !IsRmfOrZmfBytes(data, size))
		return;

	CopyRmfField(out.title, sizeof(out.title), data, size, TITLE_INFO);
	CopyRmfField(out.performed, sizeof(out.performed), data, size, PERFORMED_BY_INFO);
	CopyRmfField(out.composer, sizeof(out.composer), data, size, COMPOSER_INFO);
	CopyRmfField(out.copyright, sizeof(out.copyright), data, size, COPYRIGHT_INFO);
	CopyRmfField(out.publisher, sizeof(out.publisher), data, size, PUBLISHER_CONTACT_INFO);
	CopyRmfField(out.use_license, sizeof(out.use_license), data, size, USE_OF_LICENSE_INFO);
	CopyRmfField(out.licensed_url, sizeof(out.licensed_url), data, size, LICENSED_TO_URL_INFO);
	CopyRmfField(out.license_term, sizeof(out.license_term), data, size, LICENSE_TERM_INFO);
	CopyRmfField(out.expiration, sizeof(out.expiration), data, size, EXPIRATION_DATE_INFO);
	CopyRmfField(out.composer_notes, sizeof(out.composer_notes), data, size, COMPOSER_NOTES_INFO);
	CopyRmfField(out.index_number, sizeof(out.index_number), data, size, INDEX_NUMBER_INFO);
	CopyRmfField(out.genre, sizeof(out.genre), data, size, GENRE_INFO);
	CopyRmfField(out.sub_genre, sizeof(out.sub_genre), data, size, SUB_GENRE_INFO);
	CopyRmfField(out.tempo, sizeof(out.tempo), data, size, TEMPO_DESCRIPTION_INFO);
	CopyRmfField(out.original_source, sizeof(out.original_source), data, size, ORIGINAL_SOURCE_INFO);

	out.present = (out.title[0] || out.composer[0] || out.performed[0] || out.copyright[0]
		|| out.composer_notes[0] || out.genre[0] || out.publisher[0]
		|| out.original_source[0]) ? 1 : 0;
	/* Still mark present for IREZ/ZREZ so get_info knows we probed (even if all blank). */
	if (!out.present)
		out.present = 1;
}

void NeoBAEEngine::Prepare(const void* data, size_t size, const char* pathHint, const NeoBAEPlaybackSettings& settings, abort_callback& abort)
{
	if (!data || size == 0)
		throw exception_io_data("NeoBAE: empty file");
	abort.check();

	{
		std::lock_guard<std::recursive_mutex> lock(s_mutex);
		TeardownMixer_Locked(true);
		if (m_holdsExclusive)
			ReleaseExclusive_Locked();

		m_settings = settings;
		m_pathHint = pathHint ? pathHint : "";
		m_fileData.set_size(size);
		memcpy(m_fileData.get_ptr(), data, size);
		m_lengthSeconds = 0.0;
		m_lengthMicros = 0;
		memset(&m_rmfMetadata, 0, sizeof(m_rmfMetadata));
		SniffCodecFromData_Locked();
	}

	// Mixer-free duration + RMF/ZMF tags — safe while another session owns MusicGlobals.
	uint32_t micros = 0;
	BAEFileType ftype = BAE_INVALID_TYPE;
	BAE_RmfSongMetadata meta{};
	const BAEResult err = BAE_ProbeSongLengthFromMemory(
		m_fileData.get_ptr(),
		(uint32_t)m_fileData.get_size(),
		&micros,
		&ftype,
		&meta);
	if (err == BAE_NO_ERROR && micros > 0) {
		m_lengthMicros = micros;
		m_lengthSeconds = (double)micros / 1000000.0;
	}

	/* Prefer probe-side copy (same SongResource open as duration). If the DLL
	 * is older / probe left present=0, fall back to BAEUtil so playlist add
	 * still gets title/artist without decoding. */
	if (meta.present && (meta.title[0] || meta.composer[0] || meta.performed[0] || meta.copyright[0]))
		m_rmfMetadata = meta;
	else if (IsRmfOrZmfBytes(m_fileData.get_ptr(), m_fileData.get_size()))
		FillRmfMetadataFromUtil(m_rmfMetadata, m_fileData.get_ptr(), (uint32_t)m_fileData.get_size());
	else if (meta.present)
		m_rmfMetadata = meta;

	if (ftype != BAE_INVALID_TYPE)
		SetCodecLabel_Locked((int)ftype);
}

void NeoBAEEngine::StartDecode(bool allowLooping, abort_callback& abort, const NeoBAEPlaybackSettings* settingsRefresh)
{
	std::unique_lock<std::recursive_mutex> lock(s_mutex);
	WaitExclusive_Locked(lock, abort);
	TakeExclusive_Locked();
	abort.check();

	if (m_fileData.get_size() == 0)
		throw exception_io_data("NeoBAE: not prepared");

	if (settingsRefresh) {
		const unsigned newRate = NormalizeSampleRateHz((int64_t)settingsRefresh->sampleRateHz);
		const bool rateChanged = (newRate != m_settings.sampleRateHz);
		m_settings.loopEnabled = settingsRefresh->loopEnabled;
		m_settings.loopInfinite = settingsRefresh->loopInfinite;
		m_settings.loopCount = settingsRefresh->loopCount;
		m_settings.normalize = settingsRefresh->normalize;
		m_settings.dlsCompatibilityMode = settingsRefresh->dlsCompatibilityMode;
		m_settings.sampleRateHz = newRate;
		// Sample rate is structural — must rebuild the mixer if it changed.
		if (rateChanged && (m_mixer || m_song))
			TeardownMixer_Locked(false);
	}

	if (!m_mixer || !m_song) {
		try {
			OpenMixerAndSong_Locked(m_pathHint.get_ptr());
		} catch (...) {
			TeardownMixer_Locked(false);
			ReleaseExclusive_Locked();
			throw;
		}
	}

	BAESong song = (BAESong)m_song;
	BAEMixer mixer = (BAEMixer)m_mixer;

	ApplyDLSCompat_Locked();

	if (m_started) {
		BAESong_Stop(song, FALSE);
		BAESong_SetMicrosecondPosition(song, 0);
		m_started = false;
	}

	m_allowLooping = allowLooping && m_settings.loopEnabled;
	m_loopsDone = 0;
	m_lastPosMs = 0;

	int32_t normalizeGainPct = 100;
	if (m_settings.normalize) {
		BAEResult nerr = BAESong_NormalizeFromMidiEstimate(song, 89, &normalizeGainPct);
		if (nerr != BAE_NO_ERROR) {
			normalizeGainPct = 100;
			BAEMixer_SetSongNormalizeGain(mixer, 100);
		}
	} else {
		BAEMixer_SetSongNormalizeGain(mixer, 100);
	}

	BAESong_Preroll(song);
	BAEResult err = BAESong_Start(song, 0);
	if (err != BAE_NO_ERROR)
		ThrowBAE("BAESong_Start failed", err);

	BAEMixer_SetSongNormalizeGain(mixer, normalizeGainPct);

	if (m_allowLooping)
		BAESong_SetLoops(song, kNeoBAEInfiniteLoops);
	else
		BAESong_SetLoops(song, 0);

	m_started = true;
}

bool NeoBAEEngine::DecodeRun(audio_chunk& chunk, abort_callback& abort)
{
	std::lock_guard<std::recursive_mutex> lock(s_mutex);
	abort.check();
	if (!m_started)
		return false;

	BAESong song = (BAESong)m_song;

	// Request whole mixer slices only. NeoBAE advances MIDI by One_Slice per
	// BAE_BuildMixerSlice call; a fixed 1024-frame request is not a multiple of
	// One_Slice at 16/24/32 kHz and makes playback run fast.
	int16_t sliceFrames = BAE_GetMaxSamplePerSlice();
	if (sliceFrames <= 0)
		sliceFrames = 512;
	int32_t framesWanted = (int32_t)sliceFrames;
	// Prefer ~2 slices when small, still an exact multiple.
	if (framesWanted * 2 <= 2048)
		framesWanted *= 2;

	m_pcm.set_size((size_t)framesWanted * kChannels);
	const int32_t bytes = BAE_FB2K_RenderAudio(
		m_pcm.get_ptr(),
		(int32_t)(framesWanted * kChannels * (int)sizeof(int16_t)),
		framesWanted);
	if (bytes <= 0)
		return false;

	const t_size frames = (t_size)(bytes / (kChannels * (int)sizeof(int16_t)));
	chunk.set_data_fixedpoint(
		m_pcm.get_ptr(),
		frames * kChannels * sizeof(int16_t),
		m_settings.sampleRateHz,
		kChannels,
		kBitsPerSample,
		audio_chunk::g_guess_channel_config(kChannels));

	uint32_t posUs = 0;
	BAESong_GetMicrosecondPosition(song, &posUs);
	const uint32_t posMs = posUs / 1000;

	if (m_allowLooping && posMs < m_lastPosMs && (m_lastPosMs - posMs) > 1000) {
		++m_loopsDone;
		if (!m_settings.loopInfinite && m_loopsDone >= m_settings.loopCount) {
			BAESong_Stop(song, FALSE);
			m_started = false;
			return true;
		}
	}
	m_lastPosMs = posMs;

	if (!m_allowLooping) {
		BAE_BOOL done = FALSE;
		BAESong_IsDone(song, &done);
		if (done) {
			m_started = false;
		}
	}

	return true;
}

void NeoBAEEngine::Seek(double seconds, abort_callback& abort)
{
	std::lock_guard<std::recursive_mutex> lock(s_mutex);
	abort.check();
	BAESong song = (BAESong)m_song;
	if (!song || !m_started)
		return;

	if (seconds < 0)
		seconds = 0;
	if (!m_allowLooping && m_lengthSeconds > 0 && seconds > m_lengthSeconds)
		seconds = m_lengthSeconds;

	double target = seconds;
	if (m_allowLooping && m_lengthSeconds > 0.0) {
		target = fmod(seconds, m_lengthSeconds);
		if (target < 0)
			target += m_lengthSeconds;
	}

	const uint32_t micros = (uint32_t)(target * 1000000.0 + 0.5);
	BAESong_SetMicrosecondPosition(song, micros);
	m_lastPosMs = micros / 1000;
}
