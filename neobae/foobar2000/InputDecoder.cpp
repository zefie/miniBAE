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

	// Probe length under the exclusive mixer lock, then release the mixer so
	// playback / scanners do not keep overlapping BAEMixer sessions alive.
	m_engine.Prepare(m_data.get_ptr(), m_data.get_size(), nativePath.get_ptr(), m_settings, abort);
	m_opened = true;
}

void InputDecoder::get_info(file_info& info, abort_callback& abort)
{
	abort.check();
	if (!m_opened)
		throw exception_io_data("NeoBAE: not open");

	// Duration WITHOUT loops / fade — looping continues past EOF during playback.
	info.set_length(m_engine.GetLengthSeconds());
	info.info_set_int("samplerate", m_engine.GetSampleRate());
	info.info_set_int("channels", kChannels);
	info.info_set_int("bitspersample", kBitsPerSample);
	info.info_set("encoding", "synthesized");
	info.info_set("codec", m_engine.GetCodecName());
	info.info_set("codec_profile", "NeoBAE");
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

bool InputDecoder::decode_get_dynamic_info(file_info&, double&)
{
	return false;
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
