#include "pch.h"
#include "InputDecoder.h"

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

static void SetMetaUtf8(file_info& info, const char* name, const char* ansiValue)
{
	if (!name || !ansiValue || !ansiValue[0])
		return;
	/* RMF text is MacRoman→WinANSI from BAEUtil/probe; foobar wants UTF-8. */
	const pfc::stringcvt::string_utf8_from_ansi utf8(ansiValue);
	if (utf8.get_ptr() && utf8.get_ptr()[0])
		info.meta_set(name, utf8.get_ptr());
}

/* Map Beatnik RMF/ZMF song-info (from mixer-free Prepare probe) onto foobar tags.
 * Composer → artist, Title → title, Composer Notes → comment. */
static void FillRmfMetadata(file_info& info, const BAE_RmfSongMetadata& meta)
{
	SetMetaUtf8(info, "title", meta.title);

	if (meta.composer[0]) {
		SetMetaUtf8(info, "artist", meta.composer);
		SetMetaUtf8(info, "composer", meta.composer);
	} else if (meta.performed[0]) {
		SetMetaUtf8(info, "artist", meta.performed);
	}
	if (meta.performed[0])
		SetMetaUtf8(info, "performer", meta.performed);

	SetMetaUtf8(info, "comment", meta.composer_notes);
	SetMetaUtf8(info, "copyright", meta.copyright);
	SetMetaUtf8(info, "genre", meta.genre);
	SetMetaUtf8(info, "style", meta.sub_genre);
	SetMetaUtf8(info, "publisher", meta.publisher);
	SetMetaUtf8(info, "www", meta.licensed_url);
	SetMetaUtf8(info, "tempo", meta.tempo);
	SetMetaUtf8(info, "original source", meta.original_source);
	SetMetaUtf8(info, "index", meta.index_number);
	SetMetaUtf8(info, "license", meta.use_license);
	SetMetaUtf8(info, "license term", meta.license_term);
	SetMetaUtf8(info, "expiration", meta.expiration);
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

	if (const BAE_RmfSongMetadata* meta = m_engine.GetRmfMetadata())
		FillRmfMetadata(info, *meta);
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
