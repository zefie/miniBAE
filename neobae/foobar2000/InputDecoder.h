#pragma once

#include "pch.h"
#include "Configuration.h"
#include "NeoBAEEngine.h"

class InputDecoder : public input_stubs {
public:
	InputDecoder() = default;
	~InputDecoder() = default;

	void open(service_ptr_t<file> fileHint, const char* path, t_input_open_reason reason, abort_callback& abort);

	void get_info(file_info& info, abort_callback& abort);
	t_filestats2 get_stats2(unsigned flags, abort_callback& abort);
	t_filestats get_file_stats(abort_callback& abort);

	void decode_initialize(unsigned flags, abort_callback& abort);
	bool decode_run(audio_chunk& chunk, abort_callback& abort);
	void decode_seek(double seconds, abort_callback& abort);
	bool decode_can_seek();
	bool decode_get_dynamic_info(file_info& out, double& timestampDelta);
	bool decode_get_dynamic_info_track(file_info& out, double& timestampDelta);
	void decode_on_idle(abort_callback& abort);

	void retag(const file_info&, abort_callback&) { throw exception_tagging_unsupported(); }
	void remove_tags(abort_callback&) { throw exception_tagging_unsupported(); }

	static bool g_is_our_content_type(const char* contentType);
	static bool g_is_our_path(const char* path, const char* extension);
	static const char* g_get_name() { return "NeoBAE"; }
	static const GUID g_get_guid() { return InputDecoderGUID; }
	static GUID g_get_preferences_guid() { return PreferencesPageGUID; }

private:
	service_ptr_t<file> m_file;
	pfc::string8 m_path;
	t_filestats2 m_stats{};
	pfc::array_t<t_uint8> m_data;
	NeoBAEEngine m_engine;
	NeoBAEPlaybackSettings m_settings;
	bool m_opened = false;
	bool m_pushDynamicLength = false;
};
