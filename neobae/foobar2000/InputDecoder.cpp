#include "pch.h"
#include "InputDecoder.h"
#include "NeoBAE.h"

static bool IsNeoBAEExtension(const char* extension)
{
	if (!extension || !extension[0])
		return false;

	static const char* kExts[] = {
		"mid", "midi", "kar",
		"rmf", "zmf",
		"xmf", "mxmf",
		"rmi",
		"imy", "emy", "rng", "rtx", "rtttl",
	};
	for (const char* ext : kExts) {
		if (stricmp_utf8(extension, ext) == 0)
			return true;
	}
	return false;
}

static bool IsRmfOrZmfContainer(const t_uint8* data, size_t size)
{
	if (!data || size < 4)
		return false;
	return (data[0] == 'I' && data[1] == 'R' && data[2] == 'E' && data[3] == 'Z')
		|| (data[0] == 'Z' && data[1] == 'R' && data[2] == 'E' && data[3] == 'Z');
}

static void SetMetaUtf8(file_info& info, const char* name, const char* ansiValue)
{
	if (!name || !ansiValue || !ansiValue[0])
		return;
	/* RMF text is MacRoman → WinANSI in BAEUtil_GetRmfSongInfo; foobar wants UTF-8. */
	const pfc::stringcvt::string_utf8_from_ansi utf8(ansiValue);
	if (utf8.get_ptr() && utf8.get_ptr()[0])
		info.meta_set(name, utf8.get_ptr());
}

static void AppendCommentUtf8(file_info& info, const char* ansiValue)
{
	if (!ansiValue || !ansiValue[0])
		return;
	const pfc::stringcvt::string_utf8_from_ansi utf8(ansiValue);
	const char* text = utf8.get_ptr();
	if (!text || !text[0])
		return;
	const char* existing = info.meta_get("comment", 0);
	if (existing && existing[0]) {
		pfc::string8 merged;
		merged << existing << "\n" << text;
		info.meta_set("comment", merged.get_ptr());
	} else {
		info.meta_set("comment", text);
	}
}

/* Map Beatnik RMF/ZMF song-info fields onto foobar tags.
 * Primary credits: Composer → artist (RMF libraries browse that way), Title → title,
 * Composer Notes → comment. Remaining fields use standard or descriptive names. */
static void FillRmfMetadata(file_info& info, const void* data, size_t size)
{
	if (!data || size == 0 || size > 0x7FFFFFFFu)
		return;

	char buf[4096];
	auto readField = [&](BAEInfoType type) -> bool {
		buf[0] = 0;
		return BAEUtil_GetRmfSongInfo((void*)data, (uint32_t)size, 0, type, buf, (uint32_t)sizeof(buf)) == BAE_NO_ERROR
			&& buf[0] != 0;
	};

	if (readField(TITLE_INFO))
		SetMetaUtf8(info, "title", buf);

	char composer[4096] = {};
	char performed[4096] = {};
	if (readField(COMPOSER_INFO)) {
		strncpy(composer, buf, sizeof(composer) - 1);
		composer[sizeof(composer) - 1] = 0;
	}
	if (readField(PERFORMED_BY_INFO)) {
		strncpy(performed, buf, sizeof(performed) - 1);
		performed[sizeof(performed) - 1] = 0;
	}

	/* Composer is the usual RMF credit line → artist for library display. */
	if (composer[0]) {
		SetMetaUtf8(info, "artist", composer);
		SetMetaUtf8(info, "composer", composer);
	} else if (performed[0]) {
		SetMetaUtf8(info, "artist", performed);
	}
	if (performed[0])
		SetMetaUtf8(info, "performer", performed);

	if (readField(COMPOSER_NOTES_INFO))
		AppendCommentUtf8(info, buf);

	struct MapEntry { BAEInfoType type; const char* tag; };
	static const MapEntry kSimpleMaps[] = {
		{ COPYRIGHT_INFO, "copyright" },
		{ GENRE_INFO, "genre" },
		{ SUB_GENRE_INFO, "style" },
		{ PUBLISHER_CONTACT_INFO, "publisher" },
		{ LICENSED_TO_URL_INFO, "www" },
		{ TEMPO_DESCRIPTION_INFO, "tempo" },
		{ ORIGINAL_SOURCE_INFO, "original source" },
		{ INDEX_NUMBER_INFO, "index" },
		{ USE_OF_LICENSE_INFO, "license" },
		{ LICENSE_TERM_INFO, "license term" },
		{ EXPIRATION_DATE_INFO, "expiration" },
	};
	for (const MapEntry& entry : kSimpleMaps) {
		if (readField(entry.type))
			SetMetaUtf8(info, entry.tag, buf);
	}
}

void InputDecoder::open(service_ptr_t<file> fileHint, const char* path, t_input_open_reason reason, abort_callback& abort)
{
	if (reason == input_open_info_write)
		throw exception_tagging_unsupported();

	m_path = path;
	m_file = fileHint;
	input_open_file_helper(m_file, path, reason, abort);

	m_stats = m_file->get_stats2_(stats2_all, abort);

	const t_filesize size = m_file->get_size(abort);
	if (size == filesize_invalid || size == 0)
		throw exception_io_data("NeoBAE: empty or unreadable file");
	if (size > 64 * 1024 * 1024)
		throw exception_io_data("NeoBAE: file too large");

	m_data.set_size((size_t)size);
	m_file->seek(0, abort);
	m_file->read_object(m_data.get_ptr(), (t_size)size, abort);

	m_settings = NeoBAEPlaybackSettings::FromConfig();

	pfc::string8 nativePath;
	if (!filesystem::g_get_native_path(path, nativePath))
		nativePath = path;

	// Mixer-free duration probe (no exclusive BAEMixer / MusicGlobals).
	(void)reason;
	m_engine.Prepare(m_data.get_ptr(), m_data.get_size(), nativePath.get_ptr(), m_settings, abort);
	m_opened = true;
	m_pushDynamicLength = false;
}

void InputDecoder::get_info(file_info& info, abort_callback& abort)
{
	abort.check();
	if (!m_opened)
		throw exception_io_data("NeoBAE: not open");

	// Duration WITHOUT loops / fade — looping continues past EOF during playback.
	// Length 0 means unknown to foobar2000 (shows "?" and disables the seek bar).
	const double length = m_engine.GetLengthSeconds();
	if (length > 0.0)
		info.set_length(length);
	info.info_set_int("samplerate", m_engine.GetSampleRate());
	info.info_set_int("channels", kChannels);
	info.info_set_int("bitspersample", kBitsPerSample);
	info.info_set("encoding", "synthesized");
	info.info_set("codec", m_engine.GetCodecName());
	info.info_set("codec_profile", "NeoBAE");

	if (IsRmfOrZmfContainer(m_data.get_ptr(), m_data.get_size()))
		FillRmfMetadata(info, m_data.get_ptr(), m_data.get_size());
}

t_filestats2 InputDecoder::get_stats2(unsigned flags, abort_callback& abort)
{
	abort.check();
	return m_stats;
}

t_filestats InputDecoder::get_file_stats(abort_callback& abort)
{
	abort.check();
	return m_stats.to_legacy();
}

void InputDecoder::decode_initialize(unsigned flags, abort_callback& abort)
{
	if (!m_opened)
		throw exception_io_data("NeoBAE: not open");

	// Re-snapshot prefs for this decode session (loop / normalize / DLS compat).
	m_settings = NeoBAEPlaybackSettings::FromConfig();

	// Loop only during interactive playback, not conversion / replaygain scan.
	const bool allowLooping = (flags & input_flag_playback) != 0;
	m_engine.StartDecode(allowLooping, abort, &m_settings);
	// If Prepare couldn't publish a length (or library still has "?"), push it now.
	m_pushDynamicLength = (m_engine.GetLengthSeconds() > 0.0);
}

bool InputDecoder::decode_run(audio_chunk& chunk, abort_callback& abort)
{
	return m_engine.DecodeRun(chunk, abort);
}

void InputDecoder::decode_seek(double seconds, abort_callback& abort)
{
	m_engine.Seek(seconds, abort);
}

bool InputDecoder::decode_can_seek()
{
	return m_engine.CanSeek();
}

bool InputDecoder::decode_get_dynamic_info(file_info& info, double& timestampDelta)
{
	if (!m_pushDynamicLength)
		return false;
	const double length = m_engine.GetLengthSeconds();
	if (length <= 0.0)
		return false;
	info.set_length(length);
	timestampDelta = 0;
	m_pushDynamicLength = false;
	return true;
}

bool InputDecoder::decode_get_dynamic_info_track(file_info&, double&)
{
	return false;
}

void InputDecoder::decode_on_idle(abort_callback& abort)
{
	if (m_file.is_valid())
		m_file->on_idle(abort);
}

bool InputDecoder::g_is_our_content_type(const char* contentType)
{
	if (!contentType)
		return false;
	return stricmp_utf8(contentType, "audio/midi") == 0
		|| stricmp_utf8(contentType, "audio/x-midi") == 0
		|| stricmp_utf8(contentType, "audio/rmf") == 0
		|| stricmp_utf8(contentType, "audio/x-rmf") == 0;
}

bool InputDecoder::g_is_our_path(const char* /*path*/, const char* extension)
{
	return IsNeoBAEExtension(extension);
}

static input_singletrack_factory_t<InputDecoder> g_input_neobae_factory;

DECLARE_FILE_TYPE(
	"NeoBAE MIDI / RMF / XMF files",
	"*.MID;*.MIDI;*.KAR;*.RMF;*.ZMF;*.XMF;*.MXMF;*.RMI;*.IMY;*.EMY;*.RNG;*.RTX;*.RTTTL"
);
