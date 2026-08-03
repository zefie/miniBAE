#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>

#include "NeoBAE.h"
#include "X_Formats.h"
#include "mod2rmf_rmfcreat.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{

enum class SampleSource
{
    Bank = 0,
    Song = 1,
};

struct SampleRow
{
    uint32_t index = 0;
    uint32_t instrument_index = 0;
    uint32_t split_index = 0;
    uint32_t document_sample_index = 0;
    uint32_t snd_resource_id = 0;
    uint32_t sample_rate_hz = 44100;
    uint32_t frame_count = 0;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;
    int bit_depth = 16;
    int channels = 1;
    bool is_custom = false;
    SampleSource source = SampleSource::Bank;
    int bank = 0;
    int program = 0;
    int root_key = 60;
    int low_key = 0;
    int high_key = 127;
    std::string name;
};

struct SongRow
{
    std::string name;
    std::string path;
    std::string source_type; // MIDI / RMF / ZMF / Module
};

struct InstrumentRow
{
    bool is_custom = false;
    bool has_song_override = false;
    bool from_song_document = false;
    uint32_t instrument_index = 0;
    uint32_t inst_id = 0;
    int program = 0;
    int split_count = 0;
    int bank = 0;
    bool percussion = false;
    std::string name;
};

struct InstrumentFilterOption
{
    int bank = -1;
    int category = -1; // -1 all, 0 melodic, 1 percussion
    std::string label;
};

/* BE2 Samples tab Show: — All / Custom Samples (default) / Built-in Samples */
enum class SampleShowFilter : int
{
    All = 0,
    Custom = 1,
    BuiltIn = 2,
};

struct SampleCodecOption
{
    BAERmfEditorCompressionType type;
    const char *label;
};

static std::string MakeBankCategoryLabel(int bank, int category)
{
    char label[96];
    std::snprintf(label,
                  sizeof(label),
                  "Bank %d %s",
                  bank,
                  category == 1 ? "Percussion" : "Melodic");
    return std::string(label);
}

static std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

static std::string FileNameFromPath(const std::string &path)
{
    if (path.empty())
    {
        return std::string();
    }
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos || slash + 1 >= path.size())
    {
        return path;
    }
    return path.substr(slash + 1);
}

static bool EndsWith(const std::string &value, const char *suffix)
{
    const std::string lower = ToLower(value);
    const std::string sfx = ToLower(std::string(suffix));
    if (lower.size() < sfx.size())
    {
        return false;
    }
    return lower.compare(lower.size() - sfx.size(), sfx.size(), sfx) == 0;
}

static BAERate RateFromHz(int hz)
{
    switch (hz)
    {
    case 11025:
        return BAE_RATE_11K;
    case 22050:
        return BAE_RATE_22K;
    case 48000:
        return BAE_RATE_48K;
    case 44100:
    default:
        return BAE_RATE_44K;
    }
}

static bool IsOpusCompressionType(BAERmfEditorCompressionType compression_type)
{
    switch (compression_type)
    {
    case BAE_EDITOR_COMPRESSION_OPUS_12K:
    case BAE_EDITOR_COMPRESSION_OPUS_16K:
    case BAE_EDITOR_COMPRESSION_OPUS_24K:
    case BAE_EDITOR_COMPRESSION_OPUS_32K:
    case BAE_EDITOR_COMPRESSION_OPUS_48K:
    case BAE_EDITOR_COMPRESSION_OPUS_64K:
    case BAE_EDITOR_COMPRESSION_OPUS_80K:
    case BAE_EDITOR_COMPRESSION_OPUS_96K:
    case BAE_EDITOR_COMPRESSION_OPUS_128K:
    case BAE_EDITOR_COMPRESSION_OPUS_160K:
    case BAE_EDITOR_COMPRESSION_OPUS_192K:
    case BAE_EDITOR_COMPRESSION_OPUS_256K:
        return true;
    default:
        return false;
    }
}

static BAERmfEditorCompressionType BankCompressionFromCodec(uint32_t compression_type,
                                                            uint32_t compression_sub_type)
{
    switch (compression_type)
    {
    case 0:
    case FOUR_CHAR('n', 'o', 'n', 'e'):
        return BAE_EDITOR_COMPRESSION_PCM;
    case FOUR_CHAR('i', 'm', 'a', '4'):
    case FOUR_CHAR('i', 'm', 'a', 'W'):
    case FOUR_CHAR('i', 'm', 'a', '3'):
        return BAE_EDITOR_COMPRESSION_ADPCM;
    case FOUR_CHAR('f', 'L', 'a', 'C'):
    case FOUR_CHAR('F', 'L', 'A', 'C'):
        return BAE_EDITOR_COMPRESSION_FLAC;
    case FOUR_CHAR('m', 'p', 'g', 'n'):
        return BAE_EDITOR_COMPRESSION_MP3_32K;
    case FOUR_CHAR('m', 'p', 'g', 'b'):
        return BAE_EDITOR_COMPRESSION_MP3_48K;
    case FOUR_CHAR('m', 'p', 'g', 'd'):
        return BAE_EDITOR_COMPRESSION_MP3_64K;
    case FOUR_CHAR('m', 'p', 'g', 'f'):
        return BAE_EDITOR_COMPRESSION_MP3_96K;
    case FOUR_CHAR('m', 'p', 'g', 'h'):
        return BAE_EDITOR_COMPRESSION_MP3_128K;
    case FOUR_CHAR('m', 'p', 'g', 'j'):
        return BAE_EDITOR_COMPRESSION_MP3_192K;
    case FOUR_CHAR('m', 'p', 'g', 'l'):
        return BAE_EDITOR_COMPRESSION_MP3_256K;
    case FOUR_CHAR('m', 'p', 'g', 'm'):
        return BAE_EDITOR_COMPRESSION_MP3_320K;
    case FOUR_CHAR('O', 'g', 'g', 'V'):
    case FOUR_CHAR('V', 'O', 'R', 'B'):
        switch (compression_sub_type)
        {
        case FOUR_CHAR('v', '0', '3', '2'):
            return BAE_EDITOR_COMPRESSION_VORBIS_32K;
        case FOUR_CHAR('v', '0', '4', '8'):
            return BAE_EDITOR_COMPRESSION_VORBIS_48K;
        case FOUR_CHAR('v', '0', '6', '4'):
            return BAE_EDITOR_COMPRESSION_VORBIS_64K;
        case FOUR_CHAR('v', '0', '8', '0'):
            return BAE_EDITOR_COMPRESSION_VORBIS_80K;
        case FOUR_CHAR('v', '0', '9', '6'):
            return BAE_EDITOR_COMPRESSION_VORBIS_96K;
        case FOUR_CHAR('v', '1', '2', '8'):
            return BAE_EDITOR_COMPRESSION_VORBIS_128K;
        case FOUR_CHAR('v', '1', '6', '0'):
            return BAE_EDITOR_COMPRESSION_VORBIS_160K;
        case FOUR_CHAR('v', '1', '9', '2'):
            return BAE_EDITOR_COMPRESSION_VORBIS_192K;
        case FOUR_CHAR('v', '2', '5', '6'):
            return BAE_EDITOR_COMPRESSION_VORBIS_256K;
        default:
            return BAE_EDITOR_COMPRESSION_VORBIS_128K;
        }
    case FOUR_CHAR('O', 'g', 'g', 'O'):
    case FOUR_CHAR('O', 'P', 'U', 'S'):
        switch (compression_sub_type)
        {
        case FOUR_CHAR('o', '0', '1', '2'):
            return BAE_EDITOR_COMPRESSION_OPUS_12K;
        case FOUR_CHAR('o', '0', '1', '6'):
            return BAE_EDITOR_COMPRESSION_OPUS_16K;
        case FOUR_CHAR('o', '0', '2', '4'):
            return BAE_EDITOR_COMPRESSION_OPUS_24K;
        case FOUR_CHAR('o', '0', '3', '2'):
            return BAE_EDITOR_COMPRESSION_OPUS_32K;
        case FOUR_CHAR('o', '0', '4', '8'):
            return BAE_EDITOR_COMPRESSION_OPUS_48K;
        case FOUR_CHAR('o', '0', '6', '4'):
            return BAE_EDITOR_COMPRESSION_OPUS_64K;
        case FOUR_CHAR('o', '0', '8', '0'):
            return BAE_EDITOR_COMPRESSION_OPUS_80K;
        case FOUR_CHAR('o', '0', '9', '6'):
            return BAE_EDITOR_COMPRESSION_OPUS_96K;
        case FOUR_CHAR('o', '1', '2', '8'):
            return BAE_EDITOR_COMPRESSION_OPUS_128K;
        case FOUR_CHAR('o', '1', '6', '0'):
            return BAE_EDITOR_COMPRESSION_OPUS_160K;
        case FOUR_CHAR('o', '1', '9', '2'):
            return BAE_EDITOR_COMPRESSION_OPUS_192K;
        case FOUR_CHAR('o', '2', '5', '6'):
            return BAE_EDITOR_COMPRESSION_OPUS_256K;
        default:
            return BAE_EDITOR_COMPRESSION_OPUS_48K;
        }
    default:
        return BAE_EDITOR_COMPRESSION_PCM;
    }
}

static const SampleCodecOption *GetSampleCodecOptions(size_t *out_count)
{
    static const SampleCodecOption options[] = {
        {BAE_EDITOR_COMPRESSION_DONT_CHANGE, "No Recompression"},
        {BAE_EDITOR_COMPRESSION_PCM, "PCM"},
        {BAE_EDITOR_COMPRESSION_ADPCM, "ADPCM"},
        {BAE_EDITOR_COMPRESSION_FLAC, "FLAC"},
        {BAE_EDITOR_COMPRESSION_MP3_32K, "MP3 32k"},
        {BAE_EDITOR_COMPRESSION_MP3_48K, "MP3 48k"},
        {BAE_EDITOR_COMPRESSION_MP3_64K, "MP3 64k"},
        {BAE_EDITOR_COMPRESSION_MP3_96K, "MP3 96k"},
        {BAE_EDITOR_COMPRESSION_MP3_128K, "MP3 128k"},
        {BAE_EDITOR_COMPRESSION_MP3_192K, "MP3 192k"},
        {BAE_EDITOR_COMPRESSION_MP3_256K, "MP3 256k"},
        {BAE_EDITOR_COMPRESSION_MP3_320K, "MP3 320k"},
        {BAE_EDITOR_COMPRESSION_VORBIS_32K, "Vorbis 32k"},
        {BAE_EDITOR_COMPRESSION_VORBIS_48K, "Vorbis 48k"},
        {BAE_EDITOR_COMPRESSION_VORBIS_64K, "Vorbis 64k"},
        {BAE_EDITOR_COMPRESSION_VORBIS_80K, "Vorbis 80k"},
        {BAE_EDITOR_COMPRESSION_VORBIS_96K, "Vorbis 96k"},
        {BAE_EDITOR_COMPRESSION_VORBIS_128K, "Vorbis 128k"},
        {BAE_EDITOR_COMPRESSION_VORBIS_160K, "Vorbis 160k"},
        {BAE_EDITOR_COMPRESSION_VORBIS_192K, "Vorbis 192k"},
        {BAE_EDITOR_COMPRESSION_VORBIS_256K, "Vorbis 256k"},
        {BAE_EDITOR_COMPRESSION_OPUS_12K, "Opus 12k"},
        {BAE_EDITOR_COMPRESSION_OPUS_16K, "Opus 16k"},
        {BAE_EDITOR_COMPRESSION_OPUS_24K, "Opus 24k"},
        {BAE_EDITOR_COMPRESSION_OPUS_32K, "Opus 32k"},
        {BAE_EDITOR_COMPRESSION_OPUS_48K, "Opus 48k"},
        {BAE_EDITOR_COMPRESSION_OPUS_64K, "Opus 64k"},
        {BAE_EDITOR_COMPRESSION_OPUS_80K, "Opus 80k"},
        {BAE_EDITOR_COMPRESSION_OPUS_96K, "Opus 96k"},
        {BAE_EDITOR_COMPRESSION_OPUS_128K, "Opus 128k"},
        {BAE_EDITOR_COMPRESSION_OPUS_160K, "Opus 160k"},
        {BAE_EDITOR_COMPRESSION_OPUS_192K, "Opus 192k"},
        {BAE_EDITOR_COMPRESSION_OPUS_256K, "Opus 256k"},
    };
    if (out_count)
    {
        *out_count = SDL_arraysize(options);
    }
    return options;
}

static const char *GetSampleCodecLabel(BAERmfEditorCompressionType type)
{
    size_t option_count = 0;
    const SampleCodecOption *options = GetSampleCodecOptions(&option_count);
    for (size_t i = 0; i < option_count; ++i)
    {
        if (options[i].type == type)
        {
            return options[i].label;
        }
    }
    return "Unknown";
}

static bool IsModuleExtension(const std::string &ext)
{
    return ext == "mod" || ext == "s3m" || ext == "xm" || ext == "it" ||
           ext == "mtm" || ext == "stm" || ext == "669" || ext == "far" ||
           ext == "ult" || ext == "amf" || ext == "dbm" || ext == "imf" ||
           ext == "liq" || ext == "med" || ext == "mgt" || ext == "okt" ||
           ext == "ptm";
}

static bool IsBankExtension(const std::string &ext)
{
    return ext == "hsb" || ext == "zsb";
}

static std::string FormatBAEError(BAEResult result)
{
    switch (result)
    {
    case BAE_NO_ERROR:
        return "BAE_NO_ERROR";
    case BAE_PARAM_ERR:
        return "BAE_PARAM_ERR";
    case BAE_MEMORY_ERR:
        return "BAE_MEMORY_ERR";
    case BAE_DEVICE_UNAVAILABLE:
        return "BAE_DEVICE_UNAVAILABLE";
    case BAE_NOT_SETUP:
        return "BAE_NOT_SETUP";
    case BAE_BAD_SAMPLE_RATE:
        return "BAE_BAD_SAMPLE_RATE";
    default:
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "BAEResult(%d)", static_cast<int>(result));
        return buf;
    }
    }
}

class NbEditorApp
{
public:
    bool Init()
    {
        if (BAE_Setup() != 0)
        {
            SetStatus("BAE_Setup failed");
            return false;
        }

        m_bae_initialized = true;
        m_dialog_mutex = SDL_CreateMutex();
        if (!CreateMixer(true))
        {
            SetStatus("Audio init failed; running UI without engine");
            return true;
        }

        SetStatus("nbeditor ready");
        return true;
    }

    void Shutdown()
    {
        if (m_sample_editor_preview_sound)
        {
            BAESound_Stop(m_sample_editor_preview_sound, FALSE);
            BAESound_Delete(m_sample_editor_preview_sound);
            m_sample_editor_preview_sound = nullptr;
        }
        if (m_preview_song)
        {
            BAESong_Stop(m_preview_song, FALSE);
            BAESong_Delete(m_preview_song);
            m_preview_song = nullptr;
        }
        if (m_song)
        {
            BAESong_Stop(m_song, FALSE);
            BAESong_Delete(m_song);
            m_song = nullptr;
        }
        if (m_mixer)
        {
            BAEMixer_Close(m_mixer);
            m_mixer = nullptr;
        }
        if (m_document)
        {
            BAERmfEditorDocument_Delete(m_document);
            m_document = nullptr;
        }
        if (m_dialog_mutex)
        {
            SDL_DestroyMutex(m_dialog_mutex);
            m_dialog_mutex = nullptr;
        }
        if (m_bae_initialized)
        {
            BAE_Cleanup();
            m_bae_initialized = false;
        }
    }

    void SetMainWindow(SDL_Window *window)
    {
        m_main_window = window;
    }

    const char *GetStatus() const { return m_status; }

    void DrawUI()
    {
        ConsumeDialogResult();
        DrawMainMenuBar();
        DrawDockspace();
        DrawPlayerWindow();
        DrawSessionWindow();
        DrawSongInfoDialog();
        DrawInstrumentEditorDialog();
#if NBEDITOR_MVP
        DrawSampleEditorDialog();
#endif
    }

private:
    enum class DialogAction
    {
        OpenImport = 0,
        ExportSong,
        SaveBankAs,
    };

    static void SDLCALL OnFileDialogResult(void *userdata, const char *const *filelist, int)
    {
        NbEditorApp *app = static_cast<NbEditorApp *>(userdata);
        if (!app || !app->m_dialog_mutex)
        {
            return;
        }

        std::string selected_path;
        bool had_error = false;
        std::string error_message;

        if (!filelist)
        {
            had_error = true;
            const char *err = SDL_GetError();
            error_message = err ? err : "Unknown file dialog error";
        }
        else if (filelist[0])
        {
            selected_path = filelist[0];
        }

        SDL_LockMutex(app->m_dialog_mutex);
        app->m_dialog_result_ready = true;
        app->m_dialog_error = had_error;
        app->m_dialog_path = selected_path;
        app->m_dialog_error_message = error_message;
        SDL_UnlockMutex(app->m_dialog_mutex);
    }

    void OpenLoadDialog()
    {
        m_pending_dialog_action = DialogAction::OpenImport;
        static const SDL_DialogFileFilter filters[] = {
            {"All supported files", "rmf;zmf;mid;midi;kar;rmi;hsb;zsb;nbs;mod;s3m;xm;it;mtm;stm;669;far;ult;amf;dbm;imf;liq;med;mgt;okt;ptm;xmf"},
            {"NeoBAE Session (*.nbs)", "nbs"},
            {"RMF files (*.rmf;*.zmf)", "rmf;zmf"},
            {"Module Tracker file", "mod;s3m;xm;it;mtm;stm;669;far;ult;amf;dbm;imf;liq;med;mgt;okt;ptm;xmf"},
            {"MIDI files (*.mid;*.midi;*.kar;*.rmi)", "mid;midi;kar;rmi"},
            {"Bank files (*.hsb;*.zsb)", "hsb;zsb"},
            {"All files (*.*)", "*"},
        };

        SDL_ShowOpenFileDialog(OnFileDialogResult,
                               this,
                               m_main_window,
                               filters,
                               static_cast<int>(SDL_arraysize(filters)),
                               nullptr,
                               false);
    }

    void OpenExportSongDialog()
    {
        if (!m_document)
        {
            SetStatus("No song document to export");
            return;
        }
        m_pending_dialog_action = DialogAction::ExportSong;
        static const SDL_DialogFileFilter filters[] = {
            {"RMF / ZMF (*.rmf;*.zmf)", "rmf;zmf"},
            {"MIDI (*.mid)", "mid"},
            {"All files (*.*)", "*"},
        };
        SDL_ShowSaveFileDialog(OnFileDialogResult,
                               this,
                               m_main_window,
                               filters,
                               static_cast<int>(SDL_arraysize(filters)),
                               "song.rmf");
    }

    void OpenSaveBankDialog()
    {
        if (!m_bank_token)
        {
            SetStatus("No bank loaded to save");
            return;
        }
        m_pending_dialog_action = DialogAction::SaveBankAs;
        static const SDL_DialogFileFilter filters[] = {
            {"Bank files (*.hsb;*.zsb)", "hsb;zsb"},
            {"All files (*.*)", "*"},
        };
        const char *default_name = m_loaded_bank_path.empty() ? "bank.hsb" : FileNameFromPath(m_loaded_bank_path).c_str();
        // Keep a stable default string for the dialog lifetime.
        static char default_path[1024];
        std::snprintf(default_path, sizeof(default_path), "%s",
                      m_loaded_bank_path.empty() ? "bank.hsb" : m_loaded_bank_path.c_str());
        SDL_ShowSaveFileDialog(OnFileDialogResult,
                               this,
                               m_main_window,
                               filters,
                               static_cast<int>(SDL_arraysize(filters)),
                               default_path);
        (void)default_name;
    }

    void ConsumeDialogResult()
    {
        if (!m_dialog_mutex)
        {
            return;
        }

        bool ready = false;
        bool had_error = false;
        std::string selected_path;
        std::string error_message;

        SDL_LockMutex(m_dialog_mutex);
        if (m_dialog_result_ready)
        {
            ready = true;
            had_error = m_dialog_error;
            selected_path = m_dialog_path;
            error_message = m_dialog_error_message;
            m_dialog_result_ready = false;
            m_dialog_error = false;
            m_dialog_path.clear();
            m_dialog_error_message.clear();
        }
        SDL_UnlockMutex(m_dialog_mutex);

        if (!ready)
        {
            return;
        }

        if (had_error)
        {
            SetStatus(std::string("File dialog failed: ") + error_message);
            return;
        }

        if (selected_path.empty())
        {
            return;
        }

        if (m_pending_dialog_action == DialogAction::ExportSong)
        {
            ExportSessionSongToPath(selected_path);
            return;
        }
        if (m_pending_dialog_action == DialogAction::SaveBankAs)
        {
            if (m_bank_token)
            {
                const BAEResult save_result = BAERmfEditorBank_SaveToFile(
                    m_bank_token, const_cast<char *>(selected_path.c_str()));
                if (save_result == BAE_NO_ERROR)
                {
                    m_loaded_bank_path = selected_path;
                    m_loaded_bank_display_name = FileNameFromPath(selected_path);
                    SetStatus("Bank saved");
                }
                else
                {
                    SetStatus(std::string("Bank save failed: ") + FormatBAEError(save_result));
                }
            }
            return;
        }

        std::snprintf(m_path_input, sizeof(m_path_input), "%s", selected_path.c_str());
        ImportPathIntoSession(selected_path);
    }

    bool CreateMixer(bool initial)
    {
        BAEMixer new_mixer = BAEMixer_New();
        if (!new_mixer)
        {
            SetStatus("BAEMixer_New failed");
            return false;
        }

        BAEAudioModifiers mods = BAE_USE_16;
        if (m_stereo)
        {
            mods |= BAE_USE_STEREO;
        }

        BAEResult open_result = BAEMixer_Open(new_mixer,
                                              RateFromHz(m_sample_rate_hz),
                                              BAE_LINEAR_INTERPOLATION,
                                              mods,
                                              64,
                                              16,
                                              32,
                                              TRUE);
        if (open_result != BAE_NO_ERROR)
        {
            BAEMixer_Close(new_mixer);
            SetStatus(std::string("BAEMixer_Open failed: ") + FormatBAEError(open_result));
            return false;
        }

        BAEMixer_SetOutputGain(new_mixer, m_volume_percent);
        BAEMixer_SetDefaultReverb(new_mixer, static_cast<BAEReverbType>(m_reverb_type));

        BAESong new_preview = BAESong_New(new_mixer);
        if (!new_preview)
        {
            BAEMixer_Close(new_mixer);
            SetStatus("BAESong_New for preview failed");
            return false;
        }
        BAESong_Preroll(new_preview);

        BAESound new_sample_preview = BAESound_New(new_mixer);
        if (!new_sample_preview)
        {
            BAESong_Delete(new_preview);
            BAEMixer_Close(new_mixer);
            SetStatus("BAESound_New for sample preview failed");
            return false;
        }

        if (m_preview_song)
        {
            BAESong_Stop(m_preview_song, FALSE);
            BAESong_Delete(m_preview_song);
        }
        if (m_sample_editor_preview_sound)
        {
            BAESound_Stop(m_sample_editor_preview_sound, FALSE);
            BAESound_Delete(m_sample_editor_preview_sound);
            m_sample_editor_preview_sound = nullptr;
        }
        if (m_song)
        {
            BAESong_Stop(m_song, FALSE);
            BAESong_Delete(m_song);
            m_song = nullptr;
        }
        if (m_mixer)
        {
            BAEMixer_Close(m_mixer);
        }

        m_mixer = new_mixer;
        m_preview_song = new_preview;
    m_sample_editor_preview_sound = new_sample_preview;

        if (!LoadActiveBankIntoMixer())
        {
            SetStatus("Bank load failed");
        }
        RefreshListsFromBank();
        ApplyChannelMutes();

        if (m_document)
        {
            ReloadPlaybackFromDocument(true);
        }
        else if (!m_loaded_song_path.empty())
        {
            LoadSongFromPath(m_loaded_song_path.c_str());
        }

        if (!initial)
        {
            SetStatus("Engine reconfigured");
        }

        return true;
    }

    bool LoadBuiltinBankToMixer(BAEMixer mixer, BAEBankToken *out_token)
    {
        if (!mixer)
        {
            return false;
        }

#if _BUILT_IN_PATCHES == TRUE
        BAEBankToken token = 0;
        const BAEResult result = BAEMixer_LoadBuiltinBank(mixer, &token);
        if (out_token)
        {
            *out_token = token;
        }
        return (result == BAE_NO_ERROR && token != 0);
#else
        if (out_token)
        {
            *out_token = 0;
        }
        return false;
#endif
    }

    bool LoadActiveBankIntoMixer()
    {
        if (!m_mixer)
        {
            return false;
        }

        BAEMixer_UnloadBanks(m_mixer);

        BAEBankToken token = 0;
        if (!m_loaded_bank_path.empty())
        {
            const BAEResult bank_result = BAEMixer_AddBankFromFile(m_mixer,
                                                                   const_cast<char *>(m_loaded_bank_path.c_str()),
                                                                   &token);
            if (bank_result == BAE_NO_ERROR && token != 0)
            {
                m_bank_token = token;
                return true;
            }
            m_loaded_bank_path.clear();
            m_loaded_bank_display_name.clear();
        }

        if (LoadBuiltinBankToMixer(m_mixer, &token))
        {
            m_loaded_bank_display_name.clear();
            m_bank_token = token;
            return true;
        }

        m_bank_token = 0;
        return false;
    }

    bool LoadBankFromPath(const char *path)
    {
        if (!m_mixer || !path || !path[0])
        {
            return false;
        }

        bool restart_song = false;
        bool song_was_paused = false;
        uint32_t restore_pos_us = 0;
        std::string song_reload_path;
        if (m_song && !m_loaded_song_path.empty())
        {
            BAE_BOOL paused_flag = FALSE;
            if (BAESong_IsPaused(m_song, &paused_flag) == BAE_NO_ERROR)
            {
                song_was_paused = (paused_flag != FALSE);
            }
            (void)BAESong_GetMicrosecondPosition(m_song, &restore_pos_us);
            song_reload_path = m_loaded_song_path;
            BAESong_Stop(m_song, FALSE);
            restart_song = true;
        }

        BAEMixer_UnloadBanks(m_mixer);

        BAEBankToken token = 0;
        const BAEResult bank_result = BAEMixer_AddBankFromFile(m_mixer,
                                                               const_cast<char *>(path),
                                                               &token);
        if (bank_result != BAE_NO_ERROR || token == 0)
        {
            LoadActiveBankIntoMixer();
            SetStatus(std::string("Bank load failed: ") + FormatBAEError(bank_result));
            return false;
        }

        m_loaded_bank_path = path;
        m_loaded_bank_display_name = FileNameFromPath(m_loaded_bank_path);
        m_bank_token = token;
        RefreshListsFromBank();

        // Switch preview/player selection to the newly loaded bank.
        m_selected_uses_song_source = false;
        m_selected_bank = 0;
        BAESong audition_song = GetAuditionSong();
        if (audition_song)
        {
            BAESong_ProgramBankChange(audition_song,
                                      0,
                                      static_cast<unsigned char>(m_selected_program),
                                      static_cast<unsigned char>(m_selected_bank),
                                      0);
        }

        if (restart_song)
        {
            if (LoadSongFromPath(song_reload_path.c_str()) && m_song)
            {
                if (restore_pos_us > 0)
                {
                    BAESong_SetMicrosecondPosition(m_song, restore_pos_us);
                }
                if (song_was_paused)
                {
                    BAESong_Pause(m_song);
                }
                else
                {
                    BAESong_Resume(m_song);
                }
                m_song_started = true;
            }
            else
            {
                SetStatus("Bank loaded (song reload failed)");
                return true;
            }
        }

        SetStatus("Bank loaded");
        return true;
    }

    bool LoadBuiltinBankAsActive()
    {
        if (!m_mixer)
        {
            return false;
        }

        BAEMixer_UnloadBanks(m_mixer);
        BAEBankToken token = 0;
        if (!LoadBuiltinBankToMixer(m_mixer, &token) || token == 0)
        {
            SetStatus("Built-in bank unavailable");
            return false;
        }

        m_loaded_bank_path.clear();
        m_loaded_bank_display_name.clear();
        m_bank_token = token;
        RefreshListsFromBank();

        m_selected_uses_song_source = false;
        m_selected_bank = 0;
        BAESong audition_song = GetAuditionSong();
        if (audition_song)
        {
            BAESong_ProgramBankChange(audition_song,
                                      0,
                                      static_cast<unsigned char>(m_selected_program),
                                      static_cast<unsigned char>(m_selected_bank),
                                      0);
        }

        SetStatus("Built-in bank loaded");
        return true;
    }

    void SetStatus(const std::string &text)
    {
        std::snprintf(m_status, sizeof(m_status), "%s", text.c_str());
    }

    bool LoadSongFromPath(const char *path)
    {
        if (!m_mixer || !path || !path[0])
        {
            return false;
        }

        SetStatus("");

        const std::string path_str(path);
    const size_t ext_dot = path_str.find_last_of('.');
    const std::string ext = (ext_dot == std::string::npos) ? std::string() : ToLower(path_str.substr(ext_dot + 1));
    const bool module_like = IsModuleExtension(ext);
        const bool rmf_like = EndsWith(path_str, ".rmf") || EndsWith(path_str, ".hsb") || EndsWith(path_str, ".zmf") || EndsWith(path_str, ".zsb");

        const bool had_song = (m_song != nullptr);
        if (m_song)
        {
            BAESong_Stop(m_song, FALSE);
            BAESong_Delete(m_song);
            m_song = nullptr;
            m_loaded_song_path.clear();
            m_song_started = false;
        }

        BAESong song = BAESong_New(m_mixer);
        if (!song)
        {
            SetStatus("BAESong_New failed");
            return false;
        }

        const BAEResult load_result = LoadSongData(song, path);

        if (load_result != BAE_NO_ERROR)
        {
            BAESong_Delete(song);
            SetStatus(had_song ? std::string("Song unloaded (load failed: ") + FormatBAEError(load_result) + ")"
                              : std::string("Load failed: ") + FormatBAEError(load_result));
            return false;
        }

        BAESong_Preroll(song);
        BAESong_SetLoops(song, m_loop_enabled ? 32767 : 0);
        BAEMixer_SetOutputGain(m_mixer, m_volume_percent);

        // Prime the song engine so virtual keyboard note preview works immediately.
        BAESong_Start(song, 0);
        BAESong_Pause(song);

        m_song = song;
        m_loaded_song_path = path;
        m_song_started = true;
        RebuildPreviewSongFromPath(path);

        if (!rmf_like && !module_like)
        {
            if (m_document)
            {
                BAERmfEditorDocument_Delete(m_document);
                m_document = nullptr;
            }
            ClearSongOverrides();
        }

        ApplyChannelMutes();
        SetStatus("Song loaded");
        return true;
    }

    BAEResult LoadSongDataFromDocumentMemory(BAESong song)
    {
        if (!song || !m_document)
        {
            return BAE_PARAM_ERR;
        }

        unsigned char *rmf_data = nullptr;
        uint32_t rmf_size = 0;
        uint32_t zmf_reason = 0;
        const bool use_zmf = BAERmfEditorDocument_RequiresZmf(m_document, &zmf_reason) != FALSE;
        const BAEResult save_result = BAERmfEditorDocument_SaveAsRmfToMemory(m_document,
                                                                              use_zmf,
                                                                              &rmf_data,
                                                                              &rmf_size);
        if (save_result != BAE_NO_ERROR || !rmf_data || rmf_size == 0)
        {
            if (rmf_data)
            {
                XDisposePtr((XPTR)rmf_data);
            }
            return (save_result != BAE_NO_ERROR) ? save_result : BAE_BAD_FILE;
        }

        const BAEResult load_result = BAESong_LoadRmfFromMemory(song,
                                                                 rmf_data,
                                                                 rmf_size,
                                                                 0,
                                                                 TRUE);
        XDisposePtr((XPTR)rmf_data);
        return load_result;
    }

    BAEResult LoadSongData(BAESong song, const char *path)
    {
        if (!song || !path || !path[0])
        {
            return BAE_PARAM_ERR;
        }

        BAEResult load_result = BAE_BAD_FILE;
        const std::string path_str(path);
        if (EndsWith(path_str, ".rmf") || EndsWith(path_str, ".hsb") || EndsWith(path_str, ".zmf") || EndsWith(path_str, ".zsb"))
        {
            load_result = BAESong_LoadRmfFromFile(song, const_cast<char *>(path), 0, TRUE);
        }
        else
        {
            const size_t ext_dot = path_str.find_last_of('.');
            const std::string ext = (ext_dot == std::string::npos) ? std::string() : ToLower(path_str.substr(ext_dot + 1));
            if (IsModuleExtension(ext) && m_document)
            {
                // Module paths are converted to document form; playback uses serialized RMF/ZMF from that document.
                load_result = LoadSongDataFromDocumentMemory(song);
            }

            if (load_result == BAE_NO_ERROR)
            {
                return load_result;
            }

            load_result = BAESong_LoadMidiFromFile(song, const_cast<char *>(path), TRUE);
            if (load_result != BAE_NO_ERROR)
            {
                load_result = BAESong_LoadRmfFromFile(song, const_cast<char *>(path), 0, TRUE);
            }
        }
        return load_result;
    }

    void RebuildPreviewSongFromPath(const char *path)
    {
        if (!m_mixer)
        {
            return;
        }

        if (m_preview_song)
        {
            BAESong_Stop(m_preview_song, FALSE);
            BAESong_Delete(m_preview_song);
            m_preview_song = nullptr;
        }

        BAESong preview = BAESong_New(m_mixer);
        if (!preview)
        {
            return;
        }

        bool loaded_ok = true;
        if (path && path[0])
        {
            loaded_ok = (LoadSongData(preview, path) == BAE_NO_ERROR);
        }

        if (!loaded_ok)
        {
            BAESong_Delete(preview);
            return;
        }

        BAESong_Preroll(preview);
        m_preview_song = preview;
    }

    BAESong GetAuditionSong() const
    {
        if (m_selected_uses_song_source && m_song)
        {
            return m_song;
        }
        return m_preview_song ? m_preview_song : m_song;
    }

    void ClearSongOverrides()
    {
        m_song_override_instruments.clear();
        m_loaded_doc_path.clear();
        m_selected_uses_song_source = false;
        m_instruments = m_bank_instruments;
        RebuildInstrumentFilters();
    }

    void ApplySongOverridesToInstrumentList()
    {
        m_instruments = m_bank_instruments;

        std::map<std::pair<int, int>, size_t> by_bank_program;
        for (size_t i = 0; i < m_instruments.size(); ++i)
        {
            const InstrumentRow &inst = m_instruments[i];
            by_bank_program[std::make_pair(inst.bank, inst.program)] = i;
        }

        for (const InstrumentRow &song_inst : m_song_override_instruments)
        {
            const std::pair<int, int> key(song_inst.bank, song_inst.program);
            auto found = by_bank_program.find(key);
            if (found != by_bank_program.end())
            {
                InstrumentRow &base = m_instruments[found->second];
                base.has_song_override = true;
                base.split_count = std::max(base.split_count, song_inst.split_count);
                if (base.name.find("(Override)") == std::string::npos)
                {
                    base.name += " (Override)";
                }
            }
            else
            {
                InstrumentRow merged = song_inst;
                merged.has_song_override = true;
                if (merged.name.find("(Override)") == std::string::npos)
                {
                    merged.name += " (Override)";
                }
                by_bank_program[key] = m_instruments.size();
                m_instruments.push_back(merged);
            }
        }

        std::sort(m_instruments.begin(), m_instruments.end(), [](const InstrumentRow &a, const InstrumentRow &b) {
            if (a.bank != b.bank)
            {
                return a.bank < b.bank;
            }
            if (a.percussion != b.percussion)
            {
                return static_cast<int>(a.percussion) < static_cast<int>(b.percussion);
            }
            return a.program < b.program;
        });

        RebuildInstrumentFilters();
    }

    void RefreshSongOverridesFromDocument()
    {
        m_song_override_instruments.clear();
        if (!m_document)
        {
            ApplySongOverridesToInstrumentList();
            return;
        }

        uint32_t sample_count = 0;
        if (BAERmfEditorDocument_GetSampleCount(m_document, &sample_count) != BAE_NO_ERROR)
        {
            ApplySongOverridesToInstrumentList();
            return;
        }

        std::map<std::pair<int, int>, InstrumentRow> by_bank_program;
        for (uint32_t i = 0; i < sample_count; ++i)
        {
            BAERmfEditorSampleInfo info;
            std::memset(&info, 0, sizeof(info));
            if (BAERmfEditorDocument_GetSampleInfo(m_document, i, &info) != BAE_NO_ERROR)
            {
                continue;
            }

            uint32_t inst_id = BAE_EDITOR_INST_ID_NONE;
            (void)BAERmfEditorDocument_GetInstIDForSample(m_document, i, &inst_id);

            int bank = 2;
            int program = static_cast<int>(info.program);
            bool percussion = (program >= 128);
            if (inst_id != BAE_EDITOR_INST_ID_NONE)
            {
                bank = static_cast<int>(inst_id / 256u);
                program = static_cast<int>(inst_id % 128u);
                percussion = ((inst_id & 0x80u) != 0u);
            }

            InstrumentRow &inst = by_bank_program[std::make_pair(bank, program)];
            if (inst.split_count == 0)
            {
                inst.is_custom = true;
                inst.has_song_override = true;
                inst.program = program;
                inst.bank = bank;
                inst.percussion = percussion;
                if (info.displayName && info.displayName[0])
                {
                    inst.name = info.displayName;
                }
                else
                {
                    char generated[64];
                    std::snprintf(generated, sizeof(generated), "Inst B%dP%03d", bank, program);
                    inst.name = generated;
                }
            }
            inst.split_count += 1;
        }

        for (const auto &entry : by_bank_program)
        {
            m_song_override_instruments.push_back(entry.second);
        }

        ApplySongOverridesToInstrumentList();
    }

    bool LoadDocumentFromPath(const char *path)
    {
        if (!path || !path[0])
        {
            return false;
        }

        BAERmfEditorDocument *new_doc = BAERmfEditorDocument_LoadFromFile(const_cast<char *>(path));
        if (!new_doc)
        {
            SetStatus("RMF/ZMF document load failed (lists unchanged)");
            return false;
        }

        if (m_document)
        {
            BAERmfEditorDocument_Delete(m_document);
        }
        m_document = new_doc;
        m_loaded_doc_path = path;
        m_document_dirty = false;
        RefreshSongOverridesFromDocument();
        RefreshSongSamplesFromDocument();
        SetStatus("Document loaded");
        return true;
    }

    bool LoadModuleFromPath(const char *path)
    {
        if (!path || !path[0])
        {
            return false;
        }

#if defined(LIBXMP_STATIC) || defined(LIBXMP_CORE_PLAYER)
        BAERmfEditorDocument *new_doc = nullptr;
        const BAEResult result = mod2rmf_load_module_to_document(&new_doc, path, true);
        if (result != BAE_NO_ERROR || !new_doc)
        {
            SetStatus(std::string("Module load failed: ") + FormatBAEError(result));
            return false;
        }

        if (m_document)
        {
            BAERmfEditorDocument_Delete(m_document);
        }
        m_document = new_doc;
        m_loaded_doc_path = path;
        m_document_dirty = false;
        RefreshSongOverridesFromDocument();
        UpdateSongRowFromDocument(path, "Module");
        const bool song_ok = ReloadPlaybackFromDocument(true);
        SetStatus(song_ok ? "Module loaded" : "Module loaded (playback failed)");
        return true;
#else
        SetStatus("Tracker module support not enabled in this build");
        return false;
#endif
    }

    void RefreshListsFromBank()
    {
        m_samples.clear();
        m_instruments.clear();
        m_selected_sample = -1;
        m_selected_instrument = -1;
        RebuildInstrumentFilters();

        if (!m_bank_token)
        {
            return;
        }

        uint32_t instrument_count = 0;
        if (BAERmfEditorBank_GetInstrumentCount(m_bank_token, &instrument_count) != BAE_NO_ERROR)
        {
            return;
        }

        uint32_t sample_row_index = 0;
        for (uint32_t inst_idx = 0; inst_idx < instrument_count; ++inst_idx)
        {
            BAERmfEditorBankInstrumentInfo inst_info;
            std::memset(&inst_info, 0, sizeof(inst_info));
            if (BAERmfEditorBank_GetInstrumentInfo(m_bank_token, inst_idx, &inst_info) != BAE_NO_ERROR)
            {
                continue;
            }

            InstrumentRow inst_row;
            inst_row.is_custom = false;
            inst_row.from_song_document = false;
            inst_row.instrument_index = inst_idx;
            inst_row.inst_id = inst_info.instID;
            inst_row.program = static_cast<int>(inst_info.program);
            inst_row.split_count = (inst_info.keySplitCount <= 0) ? 1 : static_cast<int>(inst_info.keySplitCount);
            inst_row.bank = static_cast<int>(inst_info.bank);
            inst_row.percussion = ((inst_info.instID & 0x80u) != 0u);
            if (inst_info.name[0])
            {
                inst_row.name = inst_info.name;
            }
            else
            {
                char fallback[64];
                std::snprintf(fallback, sizeof(fallback), "Inst %u", inst_info.instID);
                inst_row.name = fallback;
            }
            m_instruments.push_back(inst_row);

            uint32_t split_count = 0;
            if (BAERmfEditorBank_GetInstrumentSampleCount(m_bank_token, inst_idx, &split_count) != BAE_NO_ERROR)
            {
                continue;
            }

            for (uint32_t split_idx = 0; split_idx < split_count; ++split_idx)
            {
                BAERmfEditorBankSampleInfo sample_info;
                std::memset(&sample_info, 0, sizeof(sample_info));
                if (BAERmfEditorBank_GetInstrumentSampleInfo(m_bank_token,
                                                             inst_idx,
                                                             split_idx,
                                                             &sample_info) != BAE_NO_ERROR)
                {
                    continue;
                }

                SampleRow sample_row;
                sample_row.index = sample_row_index++;
                sample_row.instrument_index = inst_idx;
                sample_row.split_index = split_idx;
                sample_row.snd_resource_id = static_cast<uint32_t>(sample_info.sndResourceID);
                sample_row.sample_rate_hz = sample_info.sampleRate;
                sample_row.frame_count = sample_info.frameCount;
                sample_row.loop_start = sample_info.loopStart;
                sample_row.loop_end = sample_info.loopEnd;
                sample_row.bit_depth = static_cast<int>(sample_info.bitDepth);
                sample_row.channels = static_cast<int>(sample_info.channels);
                sample_row.is_custom = false;
                sample_row.source = SampleSource::Bank;
                sample_row.bank = static_cast<int>(inst_info.bank);
                sample_row.program = static_cast<int>(inst_info.program);
                sample_row.root_key = static_cast<int>(sample_info.rootKey);
                sample_row.low_key = static_cast<int>(sample_info.lowKey);
                sample_row.high_key = static_cast<int>(sample_info.highKey);

                char sample_name[320];
                std::snprintf(sample_name,
                              sizeof(sample_name),
                              "%s [SND:%u]",
                              inst_row.name.c_str(),
                              static_cast<unsigned int>(sample_info.sndResourceID));
                sample_row.name = sample_name;
                m_samples.push_back(sample_row);
            }
        }

        std::sort(m_instruments.begin(), m_instruments.end(), [](const InstrumentRow &a, const InstrumentRow &b) {
            return a.program < b.program;
        });
        m_bank_instruments = m_instruments;

        if (!m_song_override_instruments.empty())
        {
            ApplySongOverridesToInstrumentList();
        }
        else
        {
            RebuildInstrumentFilters();
        }
        RefreshSongSamplesFromDocument();
    }

    void RebuildInstrumentFilters()
    {
        m_instrument_filters.clear();
        InstrumentFilterOption all;
        all.bank = -1;
        all.category = -1;
        all.label = "All";
        m_instrument_filters.push_back(all);

        const int ordered_banks[] = {0, 1, 2};
        for (int bank : ordered_banks)
        {
            InstrumentFilterOption melodic;
            melodic.bank = bank;
            melodic.category = 0;
            melodic.label = MakeBankCategoryLabel(bank, 0);
            m_instrument_filters.push_back(melodic);

            InstrumentFilterOption percussion;
            percussion.bank = bank;
            percussion.category = 1;
            percussion.label = MakeBankCategoryLabel(bank, 1);
            m_instrument_filters.push_back(percussion);
        }

        if (m_instrument_filter_index < 0 || m_instrument_filter_index >= static_cast<int>(m_instrument_filters.size()))
        {
            m_instrument_filter_index = DefaultInstrumentFilterIndex();
        }
    }

    int DefaultInstrumentFilterIndex() const
    {
        for (size_t i = 0; i < m_instrument_filters.size(); ++i)
        {
            if (m_instrument_filters[i].bank == 2 && m_instrument_filters[i].category == 0)
            {
                return static_cast<int>(i); // Bank 2 Melodic
            }
        }
        return 0;
    }

    bool InstrumentMatchesFilter(const InstrumentRow &inst) const
    {
        if (m_instrument_filter_index < 0 || m_instrument_filter_index >= static_cast<int>(m_instrument_filters.size()))
        {
            return true;
        }

        const InstrumentFilterOption &opt = m_instrument_filters[static_cast<size_t>(m_instrument_filter_index)];
        if (opt.bank < 0 && opt.category < 0)
        {
            return true;
        }
        const int category = inst.percussion ? 1 : 0;
        return inst.bank == opt.bank && category == opt.category;
    }

    void ApplyChannelMutes()
    {
        for (int ch = 0; ch < 16; ++ch)
        {
            if (m_song)
            {
                if (m_channel_muted[ch])
                {
                    BAESong_MuteChannel(m_song, static_cast<unsigned char>(ch));
                }
                else
                {
                    BAESong_UnmuteChannel(m_song, static_cast<unsigned char>(ch));
                }
            }
            if (m_preview_song)
            {
                if (m_channel_muted[ch])
                {
                    BAESong_MuteChannel(m_preview_song, static_cast<unsigned char>(ch));
                }
                else
                {
                    BAESong_UnmuteChannel(m_preview_song, static_cast<unsigned char>(ch));
                }
            }
        }
    }

    // Phase 1 Session / IE / Song Info / Export helpers
#include "nbeditor_phase1.inc"

    void DrawDockspace()
    {
        ImGuiIO &io = ImGui::GetIO();
        ImGuiViewport *viewport = ImGui::GetMainViewport();

        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(viewport->Size);
        ImGui::SetNextWindowViewport(viewport->ID);

        ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar |
                                        ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoDocking |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus |
                                        ImGuiWindowFlags_NoNavFocus |
                                        ImGuiWindowFlags_NoBackground;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

        ImGui::Begin("nbeditor_virtual_desktop", nullptr, window_flags);
        ImGui::PopStyleVar(3);

        ImGuiID dockspace_id = ImGui::GetID("nbeditor_dockspace_id");
        ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);
        SetupInitialDockLayout(dockspace_id, viewport->Size);

        ImGui::End();

        ImGui::SetNextWindowBgAlpha(0.95f);
        if (ImGui::Begin("nbeditor_status", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("%s", m_status);
            if (!m_song_row.name.empty())
            {
                ImGui::Text("Session song: %s [%s]%s",
                            m_song_row.name.c_str(),
                            m_song_row.source_type.empty() ? "?" : m_song_row.source_type.c_str(),
                            m_document_dirty ? " *" : "");
            }
            else
            {
                ImGui::Text("Session song: (none)");
            }
            ImGui::Text("Loaded path: %s", m_loaded_song_path.empty() ? "(none)" : m_loaded_song_path.c_str());
            std::string bank_display_storage;
            const char *bank_display = "Built-in Bank";
            if (!m_loaded_bank_display_name.empty())
            {
                bank_display = m_loaded_bank_display_name.c_str();
            }
            else if (!m_loaded_bank_path.empty())
            {
                bank_display = m_loaded_bank_path.c_str();
            }
            else if (m_mixer && m_bank_token)
            {
                char friendly_name[256] = {0};
                if (BAE_GetBankFriendlyName(m_mixer, m_bank_token, friendly_name, static_cast<uint32_t>(sizeof(friendly_name))) == BAE_NO_ERROR && friendly_name[0])
                {
                    bank_display_storage = friendly_name;
                    bank_display = bank_display_storage.c_str();
                }
            }
            ImGui::Text("Loaded bank: %s", bank_display);
            ImGui::Text("FPS %.1f", io.Framerate);
        }
        ImGui::End();
    }

    void SetupInitialDockLayout(ImGuiID dockspace_id, const ImVec2 &dock_size)
    {
        if (m_dock_layout_initialized)
        {
            return;
        }

        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_None);
        ImGui::DockBuilderSetNodeSize(dockspace_id, dock_size);

        ImGuiID dock_status = 0;
        ImGuiID dock_main = 0;
        ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Down, 0.14f, &dock_status, &dock_main);

        ImGuiID dock_sidebar = 0;
        ImGuiID dock_player = 0;
        ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Right, 0.38f, &dock_sidebar, &dock_player);

        ImGui::DockBuilderDockWindow("Player", dock_player);
        ImGui::DockBuilderDockWindow("Session", dock_sidebar);
        ImGui::DockBuilderDockWindow("nbeditor_status", dock_status);
        ImGui::DockBuilderFinish(dockspace_id);

        m_dock_layout_initialized = true;
    }

    void DrawPlayerWindow()
    {
        ImGui::SetNextWindowSizeConstraints(ImVec2(560.0f, 420.0f), ImVec2(4096.0f, 4096.0f));
        if (!ImGui::Begin("Player", nullptr, ImGuiWindowFlags_NoCollapse))
        {
            ImGui::End();
            return;
        }

        ImGui::InputText("Path", m_path_input, sizeof(m_path_input));
        ImGui::SameLine();
        if (ImGui::Button("Open..."))
        {
            OpenLoadDialog();
        }
        ImGui::SameLine();
        if (ImGui::Button("Load Built-in Bank"))
        {
            LoadBuiltinBankAsActive();
        }

        bool recreate = false;

        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::SliderInt("Volume", &m_volume_percent, 0, 150))
        {
            if (m_mixer)
            {
                BAEMixer_SetOutputGain(m_mixer, m_volume_percent);
            }
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Loop", &m_loop_enabled))
        {
            if (m_song)
            {
                BAESong_SetLoops(m_song, m_loop_enabled ? 32767 : 0);
            }
        }

        static const char *reverb_labels[] = {
            "None",
            "Igor's Closet",
            "Igor's Garage",
            "Igor's Acoustic Lab",
            "Igor's Cavern",
            "Igor's Dungeon",
            "Small Reflections",
            "Early Reflections",
            "Basement",
            "Banquet Hall",
            "Catacombs",
        };
        int reverb_choice = std::clamp(m_reverb_type - static_cast<int>(BAE_REVERB_TYPE_1), 0, 10);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(190.0f);
        if (ImGui::Combo("Reverb", &reverb_choice, reverb_labels, IM_ARRAYSIZE(reverb_labels)))
        {
            m_reverb_type = static_cast<int>(BAE_REVERB_TYPE_1) + reverb_choice;
            recreate = true;
        }

        ImGui::SameLine();
        bool paused = false;
        if (m_song)
        {
            BAE_BOOL paused_flag = FALSE;
            if (BAESong_IsPaused(m_song, &paused_flag) == BAE_NO_ERROR)
            {
                paused = (paused_flag != FALSE);
            }
        }
        const char *play_pause_label = (!m_song_started || paused) ? "Play" : "Pause";
        if (ImGui::Button(play_pause_label))
        {
            PlaySessionSong();
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop"))
        {
            StopSessionSong();
        }
        ImGui::SameLine();
        if (ImGui::Button("Export..."))
        {
            OpenExportSongDialog();
        }
        ImGui::SameLine();
        if (ImGui::Button("Song Info"))
        {
            OpenSongInfoDialog();
        }

        if (recreate)
        {
            CreateMixer(false);
        }

        BAEAudioInfo info;
        std::memset(&info, 0, sizeof(info));
        if (m_mixer)
        {
            BAEMixer_GetRealtimeStatus(m_mixer, &info);
        }
        UpdateKeyboardVoiceActivity(info);
        float voices_ratio = std::min(1.0f, static_cast<float>(info.voicesActive) / 128.0f);
        ImGui::Text("Voices Active: %d", static_cast<int>(info.voicesActive));
        ImGui::ProgressBar(voices_ratio, ImVec2(-1.0f, 0.0f), nullptr);

        ImGui::SeparatorText("Channel Mute");
        for (int ch = 0; ch < 16; ++ch)
        {
            char label[32];
            std::snprintf(label, sizeof(label), "Ch %02d", ch + 1);
            if (ImGui::Checkbox(label, &m_channel_muted[ch]))
            {
                ApplyChannelMutes();
            }
            if ((ch % 8) != 7)
            {
                ImGui::SameLine();
            }
        }

        if (m_song)
        {
            uint32_t pos_us = 0;
            uint32_t len_us = 0;
            BAESong_GetMicrosecondPosition(m_song, &pos_us);
            BAESong_GetMicrosecondLength(m_song, &len_us);

            uint32_t seek_us = pos_us;
            ImGui::SetNextItemWidth(-1.0f);
            if (len_us > 0 && ImGui::SliderScalar("##Position", ImGuiDataType_U32, &seek_us, &m_seek_min_us, &len_us, ""))
            {
                BAESong_SetMicrosecondPosition(m_song, seek_us);
            }

            const uint32_t pos_s = pos_us / 1000000U;
            const uint32_t len_s = len_us / 1000000U;
            ImGui::Text("%u:%02u / %u:%02u", pos_s / 60U, pos_s % 60U, len_s / 60U, len_s % 60U);
        }

        ImGui::SeparatorText("Virtual Keyboard");
        std::string selected_instrument_name;
        for (const InstrumentRow &inst : m_instruments)
        {
            if (inst.bank == m_selected_bank && inst.program == m_selected_program && !inst.name.empty())
            {
                selected_instrument_name = inst.name;
                break;
            }
        }
        if (!selected_instrument_name.empty())
        {
            ImGui::Text("B%dP%03d  %s", m_selected_bank, m_selected_program, selected_instrument_name.c_str());
        }
        else
        {
            ImGui::Text("B%dP%03d", m_selected_bank, m_selected_program);
        }
        DrawKeyboard();

        ImGui::SeparatorText("Osc");
        UpdateOscilloscopeFromMixer();
        DrawOscilloscope();

        ImGui::End();
    }

    void DrawKeyboard()
    {
        struct KeyRect
        {
            int note = 0;
            ImVec2 min;
            ImVec2 max;
        };
        static const int kFirstNote = 21;
        static const int kLastNote = 108;
        auto is_black = [](int midi_note) {
            const int pitch = midi_note % 12;
            return pitch == 1 || pitch == 3 || pitch == 6 || pitch == 8 || pitch == 10;
        };

        if (ImGui::Button("Panic"))
        {
            BAESong audition_song = GetAuditionSong();
            if (audition_song)
            {
                BAESong_AllNotesOff(audition_song, 0);
            }
            m_mouse_active_note = -1;
        }
        ImGui::SameLine();
        ImGui::Text("Click/hold keyboard keys to audition selected instrument.");

        const float width = std::max(520.0f, ImGui::GetContentRegionAvail().x);
        const float white_h = 60.0f;
        const float black_h = 38.0f;
        int white_count = 0;

        for (int note = kFirstNote; note <= kLastNote; ++note)
        {
            if (!is_black(note))
            {
                ++white_count;
            }
        }

        const float white_w = width / static_cast<float>(white_count);
        const float black_w = white_w * 0.64f;
        const ImVec2 keyboard_pos = ImGui::GetCursorScreenPos();

        ImGui::InvisibleButton("##keyboard88", ImVec2(width, white_h));
        const ImRect key_area(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImDrawList *draw = ImGui::GetWindowDrawList();

        std::vector<KeyRect> white_keys;
        std::vector<KeyRect> black_keys;
        white_keys.reserve(static_cast<size_t>(white_count));
        black_keys.reserve(36);

        int white_idx = 0;
        for (int note = kFirstNote; note <= kLastNote; ++note)
        {
            if (is_black(note))
            {
                continue;
            }
            const float x0 = keyboard_pos.x + (static_cast<float>(white_idx) * white_w);
            const float x1 = x0 + white_w;
            white_keys.push_back({note, ImVec2(x0, keyboard_pos.y), ImVec2(x1, keyboard_pos.y + white_h)});
            ++white_idx;
        }

        white_idx = 0;
        for (int note = kFirstNote; note <= kLastNote; ++note)
        {
            if (!is_black(note))
            {
                ++white_idx;
                continue;
            }
            const float center = keyboard_pos.x + (static_cast<float>(white_idx) * white_w);
            const float x0 = center - (black_w * 0.5f);
            const float x1 = center + (black_w * 0.5f);
            black_keys.push_back({note, ImVec2(x0, keyboard_pos.y), ImVec2(x1, keyboard_pos.y + black_h)});
        }

        const ImVec2 mouse = ImGui::GetIO().MousePos;
        int hovered_note = -1;
        for (const KeyRect &k : black_keys)
        {
            if (mouse.x >= k.min.x && mouse.x <= k.max.x && mouse.y >= k.min.y && mouse.y <= k.max.y)
            {
                hovered_note = k.note;
                break;
            }
        }
        if (hovered_note == -1)
        {
            for (const KeyRect &k : white_keys)
            {
                if (mouse.x >= k.min.x && mouse.x <= k.max.x && mouse.y >= k.min.y && mouse.y <= k.max.y)
                {
                    hovered_note = k.note;
                    break;
                }
            }
        }

        const bool left_clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        const bool left_down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        const bool left_released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
        const bool in_key_area = key_area.Contains(mouse);

        if (left_clicked && in_key_area && hovered_note >= 0)
        {
            if (m_mouse_active_note >= 0 && m_mouse_active_note != hovered_note)
            {
                NoteOff(m_mouse_active_note);
                m_key_mouse_held[static_cast<size_t>(m_mouse_active_note)] = false;
            }
            m_mouse_active_note = hovered_note;
            m_key_mouse_held[static_cast<size_t>(hovered_note)] = true;
            NoteOn(hovered_note);
        }
        else if (left_down && m_mouse_active_note >= 0 && in_key_area && hovered_note >= 0 && hovered_note != m_mouse_active_note)
        {
            NoteOff(m_mouse_active_note);
            m_key_mouse_held[static_cast<size_t>(m_mouse_active_note)] = false;
            m_mouse_active_note = hovered_note;
            m_key_mouse_held[static_cast<size_t>(hovered_note)] = true;
            NoteOn(hovered_note);
        }

        if (left_released && m_mouse_active_note >= 0)
        {
            NoteOff(m_mouse_active_note);
            m_key_mouse_held[static_cast<size_t>(m_mouse_active_note)] = false;
            m_mouse_active_note = -1;
        }

        const uint32_t now_ms = SDL_GetTicks();
        auto note_is_lit = [&](int note) {
            if (note < 0 || note > 127)
            {
                return false;
            }
            return m_key_mouse_held[static_cast<size_t>(note)] || m_key_active_until_ms[static_cast<size_t>(note)] > now_ms;
        };

        for (const KeyRect &k : white_keys)
        {
            const bool lit = note_is_lit(k.note);
            const ImU32 fill = lit ? IM_COL32(144, 229, 190, 255) : IM_COL32(248, 248, 248, 255);
            draw->AddRectFilled(k.min, k.max, fill, 2.0f);
            draw->AddRect(k.min, k.max, IM_COL32(34, 34, 34, 255), 2.0f);

            if ((k.note % 12) == 0)
            {
                char lbl[8];
                std::snprintf(lbl, sizeof(lbl), "C%d", (k.note / 12) - 1);
                draw->AddText(ImVec2(k.min.x + 4.0f, k.max.y - 18.0f), IM_COL32(45, 45, 45, 255), lbl);
            }
        }

        for (const KeyRect &k : black_keys)
        {
            const bool lit = note_is_lit(k.note);
            const ImU32 fill = lit ? IM_COL32(104, 196, 158, 255) : IM_COL32(24, 24, 24, 255);
            draw->AddRectFilled(k.min, k.max, fill, 2.0f);
            draw->AddRect(k.min, k.max, IM_COL32(5, 5, 5, 255), 2.0f);
        }

        ImGui::Dummy(ImVec2(width, 2.0f));
    }

    void UpdateKeyboardVoiceActivity(const BAEAudioInfo &info)
    {
        const uint32_t now_ms = SDL_GetTicks();
        const int voice_count = std::clamp(static_cast<int>(info.voicesActive), 0, static_cast<int>(BAE_MAX_VOICES));
        for (int i = 0; i < voice_count; ++i)
        {
            if (info.voiceType[i] != BAE_MIDI_PCM_VOICE)
            {
                continue;
            }
            const int note = static_cast<int>(info.midiNote[i]);
            if (note >= 0 && note < 128)
            {
                m_key_active_until_ms[static_cast<size_t>(note)] = now_ms + 120;
            }
        }
    }

    void UpdateOscilloscopeFromMixer()
    {
        if (!m_mixer)
        {
            return;
        }

        static int16_t left[16384] = {0};
        static int16_t right[16384] = {0};
        int16_t frame = 0;
        if (BAEMixer_GetAudioSampleFrame(m_mixer, left, right, &frame) != BAE_NO_ERROR)
        {
            return;
        }

        const int max_idx = static_cast<int>(SDL_arraysize(left)) - 1;
        int idx = 0;
        if (frame > 0 && frame < static_cast<int16_t>(SDL_arraysize(left)))
        {
            idx = static_cast<int>(frame) / 2;
        }
        idx = std::clamp(idx, 0, max_idx);
        const float l = static_cast<float>(left[idx]) / 32768.0f;
        const float r = static_cast<float>(right[idx]) / 32768.0f;

        // Compute a tiny local envelope so very quiet sections still animate visibly.
        float env_accum = 0.0f;
        constexpr int kEnvWindow = 48;
        for (int i = 0; i < kEnvWindow; ++i)
        {
            const int sidx = std::clamp(idx - i, 0, max_idx);
            const float sl = static_cast<float>(left[sidx]) / 32768.0f;
            const float sr = static_cast<float>(right[sidx]) / 32768.0f;
            env_accum += std::fabs((sl + sr) * 0.5f);
        }
        const float env = env_accum / static_cast<float>(kEnvWindow);
        const float dynamic_gain = std::clamp(4.4f - (env * 8.0f), 1.8f, 4.4f);
        const float mono = std::clamp(((l + r) * 0.5f) * dynamic_gain, -1.0f, 1.0f);

        m_scope_write_index = (m_scope_write_index + 1) % static_cast<int>(m_scope_history.size());
        m_scope_history[static_cast<size_t>(m_scope_write_index)] = mono;
    }

    void DrawOscilloscope()
    {
        ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
        ImVec2 canvas_size = ImVec2(ImGui::GetContentRegionAvail().x, 44.0f);
        if (canvas_size.x < 50.0f)
        {
            canvas_size.x = 50.0f;
        }

        ImDrawList *draw_list = ImGui::GetWindowDrawList();
        draw_list->AddRectFilled(canvas_pos, ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y), IM_COL32(18, 24, 32, 255));
        draw_list->AddRect(canvas_pos, ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y), IM_COL32(95, 130, 170, 255));

        const int points = static_cast<int>(m_scope_history.size());
        const float mid_y = canvas_pos.y + (canvas_size.y * 0.5f);
        const float a = 22.0f;

        ImVec2 prev = ImVec2(canvas_pos.x, mid_y);
        for (int i = 1; i < points; ++i)
        {
            const float x = canvas_pos.x + (canvas_size.x * (static_cast<float>(i) / static_cast<float>(points - 1)));
            const int history_idx = (m_scope_write_index + i) % points;
            const float y = mid_y - (a * m_scope_history[static_cast<size_t>(history_idx)]);
            ImVec2 cur = ImVec2(x, y);
            draw_list->AddLine(prev, cur, IM_COL32(118, 226, 195, 255), 1.6f);
            prev = cur;
        }

        ImGui::Dummy(canvas_size);
    }


    static bool SampleIsCustom(const SampleRow &sample)
    {
        // Song-document samples are Custom; bank-patch samples are Built-in.
        return sample.is_custom || sample.source == SampleSource::Song;
    }

    bool SampleMatchesShowFilter(const SampleRow &sample) const
    {
        switch (m_sample_show_filter)
        {
        case SampleShowFilter::Custom:
            return SampleIsCustom(sample);
        case SampleShowFilter::BuiltIn:
            return !SampleIsCustom(sample);
        case SampleShowFilter::All:
        default:
            return true;
        }
    }

    void DrawSampleListWindow()
    {
        static const char *kSampleShowLabels[] = {
            "All Samples",
            "Custom Samples",
            "Built-in Samples",
        };
        const int filter_index = static_cast<int>(m_sample_show_filter);
        const char *preview = kSampleShowLabels[filter_index >= 0 && filter_index < 3 ? filter_index : 0];

        ImGui::Text("Show:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##SampleShowFilter", preview))
        {
            for (int i = 0; i < 3; ++i)
            {
                const bool selected = (filter_index == i);
                if (ImGui::Selectable(kSampleShowLabels[i], selected))
                {
                    m_sample_show_filter = static_cast<SampleShowFilter>(i);
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        int visible_count = 0;
        for (const SampleRow &sample : m_samples)
        {
            if (SampleMatchesShowFilter(sample))
            {
                ++visible_count;
            }
        }

        ImGui::Text("Samples: %d", visible_count);
        ImGui::Separator();

        if (visible_count == 0)
        {
            ImGui::TextDisabled("No samples in this category. Try another Show: option.");
            return;
        }

        for (size_t i = 0; i < m_samples.size(); ++i)
        {
            const SampleRow &sample = m_samples[i];
            if (!SampleMatchesShowFilter(sample))
            {
                continue;
            }

            char label[320];
            std::snprintf(label,
                          sizeof(label),
                          "%s B%dP%03d  %s  [root:%d  range:%d-%d]",
                          sample.source == SampleSource::Song ? "[Song]" : "[Bank]",
                          sample.bank,
                          sample.program,
                          sample.name.c_str(),
                          sample.root_key,
                          sample.low_key,
                          sample.high_key);
            if (ImGui::Selectable(label, m_selected_sample == static_cast<int>(i)))
            {
                m_selected_sample = static_cast<int>(i);
                m_selected_uses_song_source = sample.is_custom;
                m_selected_bank = sample.bank;
                m_selected_program = sample.program;
                BAESong audition_song = GetAuditionSong();
                if (audition_song)
                {
                    BAESong_ProgramBankChange(audition_song,
                                              0,
                                              static_cast<unsigned char>(m_selected_program),
                                              static_cast<unsigned char>(m_selected_bank),
                                              0);
                }
            }

#if NBEDITOR_MVP
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                m_selected_sample = static_cast<int>(i);
                OpenSampleEditorForSelection();
            }

            char popup_id[64];
            std::snprintf(popup_id, sizeof(popup_id), "sample_ctx_%zu", i);
            if (ImGui::BeginPopupContextItem(popup_id, ImGuiPopupFlags_MouseButtonRight))
            {
                if (ImGui::MenuItem("Open Sample Editor"))
                {
                    m_selected_sample = static_cast<int>(i);
                    OpenSampleEditorForSelection();
                }
                ImGui::EndPopup();
            }
#endif
        }

    }

#if NBEDITOR_MVP
    void OpenSampleEditorForSelection()
    {
        if (m_selected_sample < 0 || m_selected_sample >= static_cast<int>(m_samples.size()))
        {
            return;
        }

        const SampleRow &sample = m_samples[static_cast<size_t>(m_selected_sample)];
        m_sample_editor_sample_row = m_selected_sample;
        m_sample_editor_is_song_sample = (sample.source == SampleSource::Song);
        m_sample_editor_document_sample_index = sample.document_sample_index;
        m_sample_editor_sample_name = sample.name;
        m_sample_editor_preview_needs_reencode = false;
        m_sample_editor_preview_note = sample.root_key;
        m_sample_editor_playing = false;
        m_sample_editor_play_started_ms = 0;
        m_sample_editor_playhead_frame = 0;
        m_sample_editor_loop_dirty = false;
        m_sample_editor_drag_loop_marker = 0;
        m_sample_editor_wave_min.clear();
        m_sample_editor_wave_max.clear();
        m_sample_editor_cached_pcm.clear();
        m_sample_editor_original_pcm.clear();

        void *wave_data = nullptr;
        uint32_t frame_count = 0;
        uint16_t bit_size = 0;
        uint16_t channels = 0;
        BAE_UNSIGNED_FIXED sample_rate = 0;
        BAERmfEditorCompressionType detected = BAE_EDITOR_COMPRESSION_PCM;
        m_sample_editor_opus_mode = BAE_EDITOR_OPUS_MODE_AUDIO;
        m_sample_editor_snd_storage_type = BAE_EDITOR_SND_STORAGE_ESND;

        if (m_sample_editor_is_song_sample)
        {
            if (!m_document)
            {
                SetStatus("Sample editor: no song document");
                return;
            }
            BAERmfEditorSampleInfo info;
            std::memset(&info, 0, sizeof(info));
            if (BAERmfEditorDocument_GetSampleInfo(m_document, sample.document_sample_index, &info) != BAE_NO_ERROR)
            {
                SetStatus("Sample editor: failed to read song sample info");
                return;
            }
            m_sample_editor_loop_start = info.sampleInfo.startLoop;
            m_sample_editor_loop_end = info.sampleInfo.endLoop;
            m_sample_editor_frame_count = info.sampleInfo.waveFrames;
            m_sample_editor_sample_rate_hz = info.sampleInfo.sampledRate
                                                ? static_cast<uint32_t>(info.sampleInfo.sampledRate >> 16)
                                                : 44100;
            m_sample_editor_channels = info.sampleInfo.channels ? info.sampleInfo.channels : 1;
            m_sample_editor_bit_depth = info.sampleInfo.bitSize ? info.sampleInfo.bitSize : 16;
            m_sample_editor_snd_storage_type = info.sndStorageType;
            m_sample_editor_opus_mode = info.opusMode;
            detected = info.compressionType;
            const void *const_wave = nullptr;
            if (BAERmfEditorDocument_GetSampleWaveformData(m_document,
                                                           sample.document_sample_index,
                                                           &const_wave,
                                                           &frame_count,
                                                           &bit_size,
                                                           &channels,
                                                           &sample_rate) != BAE_NO_ERROR ||
                !const_wave || frame_count == 0)
            {
                SetStatus("Sample editor: song waveform decode failed");
                return;
            }
            wave_data = const_cast<void *>(const_wave);
        }
        else
        {
            if (!m_bank_token)
            {
                SetStatus("Sample editor: no bank loaded");
                return;
            }
            BAERmfEditorBankSampleInfo info;
            std::memset(&info, 0, sizeof(info));
            if (BAERmfEditorBank_GetInstrumentSampleInfo(m_bank_token,
                                                         sample.instrument_index,
                                                         sample.split_index,
                                                         &info) != BAE_NO_ERROR)
            {
                SetStatus("Sample editor: failed to read sample info");
                return;
            }

            m_sample_editor_loop_start = info.loopStart;
            m_sample_editor_loop_end = info.loopEnd;
            m_sample_editor_frame_count = info.frameCount;
            m_sample_editor_sample_rate_hz = info.sampleRate;
            m_sample_editor_channels = info.channels;
            m_sample_editor_bit_depth = info.bitDepth;
            m_sample_editor_snd_storage_type = info.sndStorageType;
            detected = BankCompressionFromCodec(info.compressionType, info.compressionSubType);

            if (BAERmfEditorBank_GetSampleWaveformData(m_bank_token,
                                                       sample.instrument_index,
                                                       sample.split_index,
                                                       &wave_data,
                                                       &frame_count,
                                                       &bit_size,
                                                       &channels,
                                                       &sample_rate) != BAE_NO_ERROR ||
                !wave_data || frame_count == 0)
            {
                if (wave_data)
                {
                    BAERmfEditorBank_FreeWaveformData(wave_data);
                }
                SetStatus("Sample editor: waveform decode failed");
                return;
            }
        }

        m_sample_editor_codec_type = detected;

        size_t option_count = 0;
        const SampleCodecOption *options = GetSampleCodecOptions(&option_count);
        m_sample_editor_codec_index = 0; // No Recompression default
        for (size_t i = 0; i < option_count; ++i)
        {
            if (options[i].type == BAE_EDITOR_COMPRESSION_DONT_CHANGE)
            {
                m_sample_editor_codec_index = static_cast<int>(i);
                break;
            }
        }

        const uint32_t bytes_per_sample = static_cast<uint32_t>((bit_size / 8u) * channels);
        const uint32_t pcm_size = frame_count * bytes_per_sample;
        m_sample_editor_cached_pcm.resize(pcm_size);
        std::memcpy(m_sample_editor_cached_pcm.data(), wave_data, pcm_size);
        m_sample_editor_cached_frame_count = frame_count;
        m_sample_editor_cached_bit_size = bit_size;
        m_sample_editor_cached_channels = channels;
        m_sample_editor_cached_sample_rate = sample_rate;
        m_sample_editor_original_pcm = m_sample_editor_cached_pcm;
        m_sample_editor_original_frame_count = m_sample_editor_cached_frame_count;
        m_sample_editor_original_bit_size = m_sample_editor_cached_bit_size;
        m_sample_editor_original_channels = m_sample_editor_cached_channels;
        m_sample_editor_original_sample_rate = m_sample_editor_cached_sample_rate;
        m_sample_editor_original_codec_type = m_sample_editor_codec_type;
        m_sample_editor_original_snd_storage_type = m_sample_editor_snd_storage_type;
        m_sample_editor_original_opus_mode = m_sample_editor_opus_mode;

        const int bins = std::max(1, static_cast<int>(std::min<uint32_t>(frame_count, 2048u)));
        m_sample_editor_wave_min.assign(static_cast<size_t>(bins), 0.0f);
        m_sample_editor_wave_max.assign(static_cast<size_t>(bins), 0.0f);
        for (int bin = 0; bin < bins; ++bin)
        {
            const uint32_t begin = static_cast<uint32_t>((static_cast<uint64_t>(bin) * frame_count) / static_cast<uint64_t>(bins));
            uint32_t end = static_cast<uint32_t>((static_cast<uint64_t>(bin + 1) * frame_count) / static_cast<uint64_t>(bins));
            if (end <= begin)
            {
                end = std::min<uint32_t>(frame_count, begin + 1u);
            }
            float min_v = 1.0f;
            float max_v = -1.0f;

            if (bit_size == 16)
            {
                const int16_t *pcm16 = static_cast<const int16_t *>(wave_data);
                for (uint32_t frame = begin; frame < end; ++frame)
                {
                    float mono = 0.0f;
                    for (uint16_t ch = 0; ch < channels; ++ch)
                    {
                        const int16_t sample_value = pcm16[(frame * channels) + ch];
                        mono += static_cast<float>(sample_value) / 32768.0f;
                    }
                    mono /= static_cast<float>(std::max<uint16_t>(channels, 1));
                    min_v = std::min(min_v, mono);
                    max_v = std::max(max_v, mono);
                }
            }
            else
            {
                const uint8_t *pcm8 = static_cast<const uint8_t *>(wave_data);
                for (uint32_t frame = begin; frame < end; ++frame)
                {
                    float mono = 0.0f;
                    for (uint16_t ch = 0; ch < channels; ++ch)
                    {
                        const int sample_value = static_cast<int>(pcm8[(frame * channels) + ch]);
                        mono += (static_cast<float>(sample_value) - 128.0f) / 128.0f;
                    }
                    mono /= static_cast<float>(std::max<uint16_t>(channels, 1));
                    min_v = std::min(min_v, mono);
                    max_v = std::max(max_v, mono);
                }
            }

            m_sample_editor_wave_min[static_cast<size_t>(bin)] = min_v;
            m_sample_editor_wave_max[static_cast<size_t>(bin)] = max_v;
        }

        if (!m_sample_editor_is_song_sample)
        {
            BAERmfEditorBank_FreeWaveformData(wave_data);
        }
        m_sample_editor_open = true;
    }

    bool ApplySampleEditorEncodingToBank()
    {
        if (m_sample_editor_sample_row < 0 ||
            m_sample_editor_sample_row >= static_cast<int>(m_samples.size()) ||
            m_sample_editor_cached_pcm.empty())
        {
            return false;
        }

        const SampleRow &sample = m_samples[static_cast<size_t>(m_sample_editor_sample_row)];
        size_t option_count = 0;
        const SampleCodecOption *options = GetSampleCodecOptions(&option_count);
        if (m_sample_editor_codec_index < 0 || m_sample_editor_codec_index >= static_cast<int>(option_count))
        {
            return false;
        }

        const BAERmfEditorCompressionType target = options[static_cast<size_t>(m_sample_editor_codec_index)].type;
        const bool use_opus_roundtrip = IsOpusCompressionType(target);

        if (m_sample_editor_is_song_sample)
        {
            if (!m_document)
            {
                SetStatus("No song document for sample encode");
                return false;
            }
            BAERmfEditorSampleInfo info;
            std::memset(&info, 0, sizeof(info));
            if (BAERmfEditorDocument_GetSampleInfo(m_document, m_sample_editor_document_sample_index, &info) != BAE_NO_ERROR)
            {
                SetStatus("Song sample info read failed");
                return false;
            }
            if (target != BAE_EDITOR_COMPRESSION_DONT_CHANGE)
            {
                info.compressionType = target;
                info.sndStorageType = m_sample_editor_snd_storage_type;
                info.opusMode = m_sample_editor_opus_mode;
                info.opusRoundTripResample = use_opus_roundtrip ? TRUE : FALSE;
            }
            info.sampleInfo.startLoop = m_sample_editor_loop_start;
            info.sampleInfo.endLoop = m_sample_editor_loop_end;
            const BAEResult set_result = BAERmfEditorDocument_SetSampleInfo(
                m_document, m_sample_editor_document_sample_index, &info);
            if (set_result != BAE_NO_ERROR)
            {
                SetStatus(std::string("Song sample update failed: ") + FormatBAEError(set_result));
                return false;
            }
            if (target != BAE_EDITOR_COMPRESSION_DONT_CHANGE)
            {
                uint32_t asset_id = 0;
                if (BAERmfEditorDocument_GetSampleAssetIDForSample(m_document,
                                                                  m_sample_editor_document_sample_index,
                                                                  &asset_id) == BAE_NO_ERROR)
                {
                    (void)BAERmfEditorDocument_SetSampleAssetCompression(m_document, asset_id, target);
                }
                m_sample_editor_codec_type = target;
            }
            m_document_dirty = true;
            m_sample_editor_preview_needs_reencode = false;
            m_sample_editor_loop_dirty = false;
            RefreshSongSamplesFromDocument();
            SetStatus("Song sample updated (will re-encode on export/play)");
            return true;
        }

        if (!m_bank_token)
        {
            return false;
        }

        if (target == BAE_EDITOR_COMPRESSION_DONT_CHANGE)
        {
            if (m_sample_editor_original_pcm.empty())
            {
                SetStatus("Original sample not cached");
                return false;
            }

            const BAERmfEditorCompressionType restore_codec = m_sample_editor_original_codec_type;
            const bool restore_opus_roundtrip = IsOpusCompressionType(restore_codec);
            const BAEResult restore_result = BAERmfEditorBank_ReEncodeSampleFromPCMEx(
                m_bank_token,
                sample.instrument_index,
                sample.split_index,
                restore_codec,
                m_sample_editor_original_snd_storage_type,
                m_sample_editor_original_opus_mode,
                restore_opus_roundtrip,
                m_sample_editor_original_pcm.data(),
                m_sample_editor_original_frame_count,
                m_sample_editor_original_bit_size,
                m_sample_editor_original_channels,
                m_sample_editor_original_sample_rate);

            if (restore_result != BAE_NO_ERROR)
            {
                SetStatus(std::string("Original restore failed: ") + FormatBAEError(restore_result));
                return false;
            }

            m_sample_editor_codec_type = restore_codec;
            m_sample_editor_snd_storage_type = m_sample_editor_original_snd_storage_type;
            m_sample_editor_opus_mode = m_sample_editor_original_opus_mode;
            m_sample_editor_preview_needs_reencode = false;
            SetStatus("Sample restored to original encoding");
        }
        else
        {
            const BAEResult result = BAERmfEditorBank_ReEncodeSampleFromPCMEx(
                m_bank_token,
                sample.instrument_index,
                sample.split_index,
                target,
                m_sample_editor_snd_storage_type,
                m_sample_editor_opus_mode,
                use_opus_roundtrip,
                m_sample_editor_cached_pcm.data(),
                m_sample_editor_cached_frame_count,
                m_sample_editor_cached_bit_size,
                m_sample_editor_cached_channels,
                m_sample_editor_cached_sample_rate);

            if (result != BAE_NO_ERROR)
            {
                SetStatus(std::string("Sample encode failed: ") + FormatBAEError(result));
                return false;
            }

            m_sample_editor_codec_type = target;
        }

        BAERmfEditorBankSampleInfo updated_info;
        std::memset(&updated_info, 0, sizeof(updated_info));
        if (BAERmfEditorBank_GetInstrumentSampleInfo(m_bank_token,
                                                     sample.instrument_index,
                                                     sample.split_index,
                                                     &updated_info) == BAE_NO_ERROR)
        {
            const uint32_t max_frame = (updated_info.frameCount > 0) ? (updated_info.frameCount - 1u) : 0u;
            const uint32_t loop_start = std::min<uint32_t>(m_sample_editor_loop_start, max_frame);
            uint32_t loop_end = std::min<uint32_t>(m_sample_editor_loop_end, updated_info.frameCount);
            if (loop_end < loop_start)
            {
                loop_end = loop_start;
            }

            updated_info.loopStart = loop_start;
            updated_info.loopEnd = loop_end;
            const BAEResult loop_result = BAERmfEditorBank_SetInstrumentSampleInfo(m_bank_token,
                                                                                    sample.instrument_index,
                                                                                    sample.split_index,
                                                                                    &updated_info);
            if (loop_result != BAE_NO_ERROR)
            {
                SetStatus(std::string("Loop update failed: ") + FormatBAEError(loop_result));
                return false;
            }

            m_sample_editor_loop_start = loop_start;
            m_sample_editor_loop_end = loop_end;
            m_sample_editor_frame_count = updated_info.frameCount;
        }

        void *wave_data = nullptr;
        uint32_t frame_count = 0;
        uint16_t bit_size = 0;
        uint16_t channels = 0;
        BAE_UNSIGNED_FIXED sample_rate = 0;
        const BAEResult waveform_result = BAERmfEditorBank_GetSampleWaveformData(m_bank_token,
                                                                                  sample.instrument_index,
                                                                                  sample.split_index,
                                                                                  &wave_data,
                                                                                  &frame_count,
                                                                                  &bit_size,
                                                                                  &channels,
                                                                                  &sample_rate);
        if (waveform_result == BAE_NO_ERROR && wave_data && frame_count > 0)
        {
            const uint32_t bytes_per_sample = static_cast<uint32_t>((bit_size / 8u) * channels);
            const uint32_t pcm_size = frame_count * bytes_per_sample;
            m_sample_editor_cached_pcm.resize(pcm_size);
            std::memcpy(m_sample_editor_cached_pcm.data(), wave_data, pcm_size);
            m_sample_editor_cached_frame_count = frame_count;
            m_sample_editor_cached_bit_size = bit_size;
            m_sample_editor_cached_channels = channels;
            m_sample_editor_cached_sample_rate = sample_rate;
            BAERmfEditorBank_FreeWaveformData(wave_data);
        }
        else if (wave_data)
        {
            BAERmfEditorBank_FreeWaveformData(wave_data);
        }

        m_sample_editor_preview_needs_reencode = false;
        m_sample_editor_loop_dirty = false;
        RefreshListsFromBank();
        return true;
    }

    void StopSampleEditorPreview()
    {
        if (!m_sample_editor_playing)
        {
            return;
        }
        if (m_sample_editor_preview_sound)
        {
            BAESound_Stop(m_sample_editor_preview_sound, FALSE);
        }
        m_sample_editor_playing = false;
    }

    void DrawSampleEditorWaveform()
    {
        ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
        ImVec2 canvas_size = ImVec2(ImGui::GetContentRegionAvail().x, 220.0f);
        if (canvas_size.x < 180.0f)
        {
            canvas_size.x = 180.0f;
        }

        ImDrawList *draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(canvas_pos,
                            ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
                            IM_COL32(18, 20, 30, 255),
                            4.0f);
        draw->AddRect(canvas_pos,
                      ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
                      IM_COL32(84, 104, 128, 255),
                      4.0f,
                      0,
                      1.2f);

        if (m_sample_editor_frame_count > 0)
        {
            const float loop_x0 = canvas_pos.x + (canvas_size.x * (static_cast<float>(m_sample_editor_loop_start) / static_cast<float>(m_sample_editor_frame_count)));
            const float loop_x1 = canvas_pos.x + (canvas_size.x * (static_cast<float>(m_sample_editor_loop_end) / static_cast<float>(m_sample_editor_frame_count)));
            if (m_sample_editor_loop_end > m_sample_editor_loop_start)
            {
                draw->AddRectFilled(ImVec2(loop_x0, canvas_pos.y),
                                    ImVec2(loop_x1, canvas_pos.y + canvas_size.y),
                                    IM_COL32(74, 180, 136, 56));
                draw->AddLine(ImVec2(loop_x0, canvas_pos.y), ImVec2(loop_x0, canvas_pos.y + canvas_size.y), IM_COL32(124, 244, 194, 168), 1.5f);
                draw->AddLine(ImVec2(loop_x1, canvas_pos.y), ImVec2(loop_x1, canvas_pos.y + canvas_size.y), IM_COL32(124, 244, 194, 168), 1.5f);
            }
        }

        const float center_y = canvas_pos.y + (canvas_size.y * 0.5f);
        draw->AddLine(ImVec2(canvas_pos.x, center_y), ImVec2(canvas_pos.x + canvas_size.x, center_y), IM_COL32(66, 84, 105, 255), 1.0f);

        const size_t bins = std::min(m_sample_editor_wave_min.size(), m_sample_editor_wave_max.size());
        for (size_t i = 0; i < bins; ++i)
        {
            const float x0 = canvas_pos.x + (canvas_size.x * (static_cast<float>(i) / static_cast<float>(std::max<size_t>(bins, 1))));
            float x1 = canvas_pos.x + (canvas_size.x * (static_cast<float>(i + 1) / static_cast<float>(std::max<size_t>(bins, 1))));
            if (x1 <= x0)
            {
                x1 = x0 + 1.0f;
            }
            const float y0 = center_y - (m_sample_editor_wave_max[i] * (canvas_size.y * 0.45f));
            const float y1 = center_y - (m_sample_editor_wave_min[i] * (canvas_size.y * 0.45f));
            draw->AddRectFilled(ImVec2(x0, std::min(y0, y1)),
                                ImVec2(x1, std::max(y0, y1)),
                                IM_COL32(108, 186, 246, 255));
        }

        if (m_sample_editor_frame_count > 0)
        {
            const float play_x = canvas_pos.x + (canvas_size.x * (static_cast<float>(m_sample_editor_playhead_frame) / static_cast<float>(m_sample_editor_frame_count)));
            draw->AddLine(ImVec2(play_x, canvas_pos.y),
                          ImVec2(play_x, canvas_pos.y + canvas_size.y),
                          IM_COL32(255, 238, 138, 255),
                          2.0f);
        }

        ImGui::SetCursorScreenPos(canvas_pos);
        ImGui::InvisibleButton("##SampleWaveformCanvas", canvas_size);

        if (m_sample_editor_frame_count > 0)
        {
            const float loop_x0 = canvas_pos.x + (canvas_size.x * (static_cast<float>(m_sample_editor_loop_start) / static_cast<float>(m_sample_editor_frame_count)));
            const float loop_x1 = canvas_pos.x + (canvas_size.x * (static_cast<float>(m_sample_editor_loop_end) / static_cast<float>(m_sample_editor_frame_count)));

            const ImVec2 mouse = ImGui::GetIO().MousePos;
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                const float dist_start = std::fabs(mouse.x - loop_x0);
                const float dist_end = std::fabs(mouse.x - loop_x1);
                if (dist_start <= 8.0f || dist_end <= 8.0f)
                {
                    m_sample_editor_drag_loop_marker = (dist_start <= dist_end) ? 1 : 2;
                }
            }

            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                m_sample_editor_drag_loop_marker = 0;
            }

            if (m_sample_editor_drag_loop_marker != 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                float t = (mouse.x - canvas_pos.x) / std::max(canvas_size.x, 1.0f);
                t = std::clamp(t, 0.0f, 1.0f);
                const uint32_t frame = static_cast<uint32_t>(t * static_cast<float>(m_sample_editor_frame_count));

                if (m_sample_editor_drag_loop_marker == 1)
                {
                    const uint32_t new_start = std::min<uint32_t>(frame, m_sample_editor_loop_end);
                    if (new_start != m_sample_editor_loop_start)
                    {
                        m_sample_editor_loop_start = new_start;
                        m_sample_editor_loop_dirty = true;
                    }
                }
                else
                {
                    const uint32_t new_end = std::max<uint32_t>(frame, m_sample_editor_loop_start);
                    if (new_end != m_sample_editor_loop_end)
                    {
                        m_sample_editor_loop_end = new_end;
                        m_sample_editor_loop_dirty = true;
                    }
                }
            }
        }
    }

    void DrawSampleEditorDialog()
    {
        if (!m_sample_editor_open)
        {
            return;
        }

        bool open = m_sample_editor_open;
        ImGui::SetNextWindowSize(ImVec2(900.0f, 620.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Sample Editor", &open, ImGuiWindowFlags_NoCollapse))
        {
            ImGui::End();
            if (!open && m_sample_editor_open)
            {
                StopSampleEditorPreview();
            }
            m_sample_editor_open = open;
            return;
        }

        if (m_sample_editor_sample_row >= 0 && m_sample_editor_sample_row < static_cast<int>(m_samples.size()))
        {
            const SampleRow &sample = m_samples[static_cast<size_t>(m_sample_editor_sample_row)];
            ImGui::Text("%s", m_sample_editor_sample_name.c_str());
            ImGui::Text("B%dP%03d  root:%d  range:%d-%d", sample.bank, sample.program, sample.root_key, sample.low_key, sample.high_key);
            ImGui::Text("%u Hz  %d-bit  %dch  frames:%u", m_sample_editor_sample_rate_hz, m_sample_editor_bit_depth, m_sample_editor_channels, m_sample_editor_frame_count);
            ImGui::Text("Current Codec: %s", GetSampleCodecLabel(m_sample_editor_codec_type));
        }
        ImGui::Separator();

        size_t option_count = 0;
        const SampleCodecOption *options = GetSampleCodecOptions(&option_count);
        if (m_sample_editor_codec_index < 0 || m_sample_editor_codec_index >= static_cast<int>(option_count))
        {
            m_sample_editor_codec_index = 0;
        }

        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::BeginCombo("Compression", options[static_cast<size_t>(m_sample_editor_codec_index)].label))
        {
            for (size_t i = 0; i < option_count; ++i)
            {
                const bool selected = (m_sample_editor_codec_index == static_cast<int>(i));
                if (ImGui::Selectable(options[i].label, selected))
                {
                    m_sample_editor_codec_index = static_cast<int>(i);
                    m_sample_editor_preview_needs_reencode = true;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        static const char *storage_labels[] = {"ESND", "CSND", "SND"};
        int storage_index = std::clamp(static_cast<int>(m_sample_editor_snd_storage_type), 0, 2);
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::Combo("Storage", &storage_index, storage_labels, IM_ARRAYSIZE(storage_labels)))
        {
            m_sample_editor_snd_storage_type = static_cast<BAERmfEditorSndStorageType>(storage_index);
            m_sample_editor_preview_needs_reencode = true;
        }

        ImGui::SameLine();
        static const char *opus_mode_labels[] = {"Opus Audio", "Opus Voice"};
        int opus_mode = std::clamp(static_cast<int>(m_sample_editor_opus_mode), 0, 1);
        ImGui::SetNextItemWidth(130.0f);
        if (ImGui::Combo("Opus Mode", &opus_mode, opus_mode_labels, IM_ARRAYSIZE(opus_mode_labels)))
        {
            m_sample_editor_opus_mode = static_cast<BAERmfEditorOpusMode>(opus_mode);
            m_sample_editor_preview_needs_reencode = true;
        }

        if (ImGui::Button("Apply Compression"))
        {
            ApplySampleEditorEncodingToBank();
        }

        ImGui::SameLine();
        if (ImGui::Button("Save"))
        {
            bool applied_ok = true;
            if (m_sample_editor_preview_needs_reencode || m_sample_editor_loop_dirty)
            {
                applied_ok = ApplySampleEditorEncodingToBank();
            }

            if (applied_ok)
            {
                if (m_loaded_bank_path.empty())
                {
                    SetStatus("Bank updated in memory (no file path to save)");
                }
                else
                {
                    const BAEResult save_result = BAERmfEditorBank_SaveToFile(m_bank_token,
                                                                              const_cast<char *>(m_loaded_bank_path.c_str()));
                    if (save_result == BAE_NO_ERROR)
                    {
                        SetStatus(std::string("Bank saved: ") + FileNameFromPath(m_loaded_bank_path));
                    }
                    else
                    {
                        SetStatus(std::string("Bank save failed: ") + FormatBAEError(save_result));
                    }
                }
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Play"))
        {
            if (m_sample_editor_preview_needs_reencode)
            {
                (void)ApplySampleEditorEncodingToBank();
            }

            if (m_sample_editor_cached_frame_count > 0 &&
                !m_sample_editor_cached_pcm.empty() &&
                m_sample_editor_preview_sound)
            {
                BAESound_Stop(m_sample_editor_preview_sound, FALSE);

                const uint32_t loop_start = std::min<uint32_t>(m_sample_editor_loop_start, m_sample_editor_cached_frame_count);
                const uint32_t loop_end = std::min<uint32_t>(m_sample_editor_loop_end, m_sample_editor_cached_frame_count);

                BAEResult result = BAESound_LoadCustomSample(
                    m_sample_editor_preview_sound,
                    m_sample_editor_cached_pcm.data(),
                    m_sample_editor_cached_frame_count,
                    m_sample_editor_cached_bit_size,
                    m_sample_editor_cached_channels,
                    m_sample_editor_cached_sample_rate,
                    loop_start,
                    loop_end);

                if (result == BAE_NO_ERROR)
                {
                    (void)BAESound_SetLoopCount(m_sample_editor_preview_sound, 0);
                    result = BAESound_Start(m_sample_editor_preview_sound,
                                            0,
                                            LONG_TO_UNSIGNED_FIXED(1),
                                            0);
                }

                if (result == BAE_NO_ERROR)
                {
                    m_sample_editor_play_started_ms = SDL_GetTicks();
                    m_sample_editor_playhead_frame = 0;
                    m_sample_editor_playing = true;
                    SetStatus("Sample previewing raw PCM at native rate");
                }
                else
                {
                    m_sample_editor_playing = false;
                    SetStatus(std::string("Sample preview failed: ") + FormatBAEError(result));
                }
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Stop"))
        {
            StopSampleEditorPreview();
            m_sample_editor_playhead_frame = 0;
        }

        uint32_t loop_start_input = m_sample_editor_loop_start;
        uint32_t loop_end_input = m_sample_editor_loop_end;
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::InputScalar("Loop Start", ImGuiDataType_U32, &loop_start_input))
        {
            const uint32_t max_frame = (m_sample_editor_frame_count > 0) ? (m_sample_editor_frame_count - 1u) : 0u;
            loop_start_input = std::min<uint32_t>(loop_start_input, max_frame);
            if (loop_start_input > m_sample_editor_loop_end)
            {
                m_sample_editor_loop_end = loop_start_input;
            }
            if (loop_start_input != m_sample_editor_loop_start)
            {
                m_sample_editor_loop_start = loop_start_input;
                m_sample_editor_loop_dirty = true;
            }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::InputScalar("Loop End", ImGuiDataType_U32, &loop_end_input))
        {
            loop_end_input = std::min<uint32_t>(loop_end_input, m_sample_editor_frame_count);
            if (loop_end_input < m_sample_editor_loop_start)
            {
                m_sample_editor_loop_start = loop_end_input;
            }
            if (loop_end_input != m_sample_editor_loop_end)
            {
                m_sample_editor_loop_end = loop_end_input;
                m_sample_editor_loop_dirty = true;
            }
        }
        if (m_sample_editor_loop_dirty)
        {
            ImGui::SameLine();
            ImGui::TextUnformatted("(loop changed)");
        }

        if (m_sample_editor_playing)
        {
            if (m_sample_editor_preview_sound)
            {
                BAE_BOOL is_done = FALSE;
                if (BAESound_IsDone(m_sample_editor_preview_sound, &is_done) == BAE_NO_ERROR && is_done)
                {
                    m_sample_editor_playing = false;
                }
            }

            const uint32_t elapsed_ms = SDL_GetTicks() - m_sample_editor_play_started_ms;
            const uint32_t native_hz = UNSIGNED_FIXED_TO_LONG_ROUNDED(m_sample_editor_cached_sample_rate);
            const uint64_t elapsed_frames = (static_cast<uint64_t>(elapsed_ms) * static_cast<uint64_t>(std::max<uint32_t>(native_hz, 1))) / 1000ull;
            if (m_sample_editor_frame_count > 0)
            {
                if (m_sample_editor_loop_end > m_sample_editor_loop_start && m_sample_editor_loop_end <= m_sample_editor_frame_count)
                {
                    if (elapsed_frames < m_sample_editor_loop_end)
                    {
                        m_sample_editor_playhead_frame = static_cast<uint32_t>(elapsed_frames);
                    }
                    else
                    {
                        const uint32_t loop_len = m_sample_editor_loop_end - m_sample_editor_loop_start;
                        const uint32_t loop_pos = static_cast<uint32_t>((elapsed_frames - m_sample_editor_loop_start) % std::max<uint32_t>(loop_len, 1));
                        m_sample_editor_playhead_frame = m_sample_editor_loop_start + loop_pos;
                    }
                }
                else
                {
                    m_sample_editor_playhead_frame = static_cast<uint32_t>(std::min<uint64_t>(elapsed_frames, m_sample_editor_frame_count));
                    if (elapsed_frames >= m_sample_editor_frame_count)
                    {
                        StopSampleEditorPreview();
                    }
                }
            }
        }

        DrawSampleEditorWaveform();

        if (ImGui::Button("Close"))
        {
            StopSampleEditorPreview();
            open = false;
        }

        ImGui::End();
        if (!open && m_sample_editor_open)
        {
            StopSampleEditorPreview();
        }
        m_sample_editor_open = open;
    }
#endif

    void DrawInstrumentListWindow()
    {
        if (!m_instrument_filters.empty())
        {
            ImGui::Text("Show:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            const char *preview = m_instrument_filters[static_cast<size_t>(m_instrument_filter_index)].label.c_str();
            if (ImGui::BeginCombo("##InstrumentFilter", preview))
            {
                for (size_t i = 0; i < m_instrument_filters.size(); ++i)
                {
                    const bool selected = (m_instrument_filter_index == static_cast<int>(i));
                    if (ImGui::Selectable(m_instrument_filters[i].label.c_str(), selected))
                    {
                        m_instrument_filter_index = static_cast<int>(i);
                    }
                    if (selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
        }

        int visible_count = 0;
        for (size_t i = 0; i < m_instruments.size(); ++i)
        {
            const InstrumentRow &inst = m_instruments[i];
            if (InstrumentMatchesFilter(inst))
            {
                ++visible_count;
            }
        }

        ImGui::Text("Instruments: %d", visible_count);
        ImGui::Separator();

        for (size_t i = 0; i < m_instruments.size(); ++i)
        {
            const InstrumentRow &inst = m_instruments[i];
            if (!InstrumentMatchesFilter(inst))
            {
                continue;
            }

            char label[256];
            std::snprintf(label,
                          sizeof(label),
                          "B%dP%03d  %s  (%d split%s)",
                          inst.bank,
                          inst.program,
                          inst.name.c_str(),
                          inst.split_count,
                          inst.split_count == 1 ? "" : "s");
            if (ImGui::Selectable(label, m_selected_instrument == static_cast<int>(i)))
            {
                m_selected_instrument = static_cast<int>(i);
                m_selected_uses_song_source = inst.is_custom || inst.has_song_override;
                m_selected_bank = inst.bank;
                m_selected_program = inst.program;
                BAESong audition_song = GetAuditionSong();
                if (audition_song)
                {
                    BAESong_ProgramBankChange(audition_song,
                                              0,
                                              static_cast<unsigned char>(m_selected_program),
                                              static_cast<unsigned char>(m_selected_bank),
                                              0);
                }
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                m_selected_instrument = static_cast<int>(i);
                OpenInstrumentEditorForSelection();
            }
            char popup_id[64];
            std::snprintf(popup_id, sizeof(popup_id), "inst_ctx_%zu", i);
            if (ImGui::BeginPopupContextItem(popup_id, ImGuiPopupFlags_MouseButtonRight))
            {
                if (ImGui::MenuItem("Edit Instrument"))
                {
                    m_selected_instrument = static_cast<int>(i);
                    OpenInstrumentEditorForSelection();
                }
                ImGui::EndPopup();
            }
        }

    }

    void NoteOn(int midi_note)
    {
        BAESong audition_song = GetAuditionSong();
        if (!audition_song)
        {
            return;
        }

        BAESong_ProgramBankChange(audition_song,
                      0,
                      static_cast<unsigned char>(m_selected_program),
                      static_cast<unsigned char>(m_selected_bank),
                      0);
        BAESong_NoteOnWithLoad(audition_song,
                               0,
                               static_cast<unsigned char>(midi_note),
                               100,
                               0);
    }

    void NoteOff(int midi_note)
    {
        BAESong audition_song = GetAuditionSong();
        if (!audition_song)
        {
            return;
        }
        BAESong_NoteOff(audition_song,
                        0,
                        static_cast<unsigned char>(midi_note),
                        0,
                        0);
    }

private:
    bool m_bae_initialized = false;
    BAEMixer m_mixer = nullptr;
    BAESong m_song = nullptr;
    BAESong m_preview_song = nullptr;
    BAERmfEditorDocument *m_document = nullptr;
    BAEBankToken m_bank_token = 0;

    std::vector<SampleRow> m_samples;
    std::vector<InstrumentRow> m_instruments;
    std::vector<InstrumentRow> m_bank_instruments;
    std::vector<InstrumentRow> m_song_override_instruments;
    std::vector<InstrumentFilterOption> m_instrument_filters;

    int m_selected_sample = -1;
    int m_selected_instrument = -1;
    int m_instrument_filter_index = 5; // Bank 2 Melodic (All=0 … Bank2 Melodic=5)
    SampleShowFilter m_sample_show_filter = SampleShowFilter::Custom;
    bool m_selected_uses_song_source = false;
    int m_selected_bank = 0;
    int m_selected_program = 0;
    bool m_song_started = false;

    int m_sample_rate_hz = 44100;
    int m_sample_rate_index = 2;
    bool m_stereo = true;
    int m_reverb_type = static_cast<int>(BAE_REVERB_TYPE_1);
    int m_volume_percent = 100;
    bool m_loop_enabled = false;

    std::array<bool, 16> m_channel_muted = {};
    std::array<bool, 128> m_key_mouse_held = {};
    std::array<uint32_t, 128> m_key_active_until_ms = {};
    int m_mouse_active_note = -1;

    std::array<float, 128> m_scope_history = {};
    int m_scope_write_index = 0;
    uint32_t m_seek_min_us = 0;

#if NBEDITOR_MVP
    bool m_sample_editor_open = false;
    bool m_sample_editor_is_song_sample = false;
    uint32_t m_sample_editor_document_sample_index = 0;
    int m_sample_editor_sample_row = -1;
    bool m_sample_editor_preview_needs_reencode = false;
    bool m_sample_editor_playing = false;
    int m_sample_editor_preview_note = 60;
    uint32_t m_sample_editor_play_started_ms = 0;
    uint32_t m_sample_editor_playhead_frame = 0;
    uint32_t m_sample_editor_frame_count = 0;
    uint32_t m_sample_editor_sample_rate_hz = 44100;
    uint32_t m_sample_editor_loop_start = 0;
    uint32_t m_sample_editor_loop_end = 0;
    int m_sample_editor_channels = 1;
    int m_sample_editor_bit_depth = 16;
    int m_sample_editor_codec_index = 0;
    BAERmfEditorCompressionType m_sample_editor_codec_type = BAE_EDITOR_COMPRESSION_PCM;
    BAERmfEditorSndStorageType m_sample_editor_snd_storage_type = BAE_EDITOR_SND_STORAGE_ESND;
    BAERmfEditorOpusMode m_sample_editor_opus_mode = BAE_EDITOR_OPUS_MODE_AUDIO;
    std::string m_sample_editor_sample_name;
    std::vector<float> m_sample_editor_wave_min;
    std::vector<float> m_sample_editor_wave_max;
    std::vector<unsigned char> m_sample_editor_cached_pcm;
    uint32_t m_sample_editor_cached_frame_count = 0;
    uint16_t m_sample_editor_cached_bit_size = 0;
    uint16_t m_sample_editor_cached_channels = 0;
    BAE_UNSIGNED_FIXED m_sample_editor_cached_sample_rate = 0;
    std::vector<unsigned char> m_sample_editor_original_pcm;
    uint32_t m_sample_editor_original_frame_count = 0;
    uint16_t m_sample_editor_original_bit_size = 0;
    uint16_t m_sample_editor_original_channels = 0;
    BAE_UNSIGNED_FIXED m_sample_editor_original_sample_rate = 0;
    BAERmfEditorCompressionType m_sample_editor_original_codec_type = BAE_EDITOR_COMPRESSION_PCM;
    BAERmfEditorSndStorageType m_sample_editor_original_snd_storage_type = BAE_EDITOR_SND_STORAGE_ESND;
    BAERmfEditorOpusMode m_sample_editor_original_opus_mode = BAE_EDITOR_OPUS_MODE_AUDIO;
    bool m_sample_editor_loop_dirty = false;
    int m_sample_editor_drag_loop_marker = 0;
    BAESound m_sample_editor_preview_sound = nullptr;
#endif

    char m_status[256] = {0};
    char m_path_input[1024] = {0};
    std::string m_loaded_song_path;
    std::string m_loaded_doc_path;
    std::string m_loaded_bank_path;
    std::string m_loaded_bank_display_name;

    SDL_Window *m_main_window = nullptr; // not owned
    SDL_Mutex *m_dialog_mutex = nullptr;
    bool m_dialog_result_ready = false;
    bool m_dialog_error = false;
    std::string m_dialog_path;
    std::string m_dialog_error_message;
    DialogAction m_pending_dialog_action = DialogAction::OpenImport;

    SongRow m_song_row;
    bool m_document_dirty = false;
    bool m_request_quit = false;

    bool m_song_info_open = false;
    char m_song_info_title[256] = {0};
    char m_song_info_composer[256] = {0};
    char m_song_info_copyright[256] = {0};
    char m_song_info_performed[256] = {0};
    int m_song_info_bpm = 120;
    int m_song_info_tpq = 480;
    bool m_song_info_loop_enabled = false;
    int m_song_info_loop_start = 0;
    int m_song_info_loop_end = 0;
    int m_song_info_loop_count = 0;
    int m_song_info_storage = 1;

    bool m_instrument_editor_open = false;
    int m_ie_page = 0;
    bool m_ie_dirty = false;
    bool m_ie_from_song = false;
    uint32_t m_ie_inst_id = 0;
    uint32_t m_ie_instrument_index = 0;
    int m_ie_bank = 0;
    int m_ie_program = 0;
    int m_ie_selected_split = -1;
    BAERmfEditorInstrumentExtInfo m_ie_ext = {};

    bool m_dock_layout_initialized = false;

public:
    bool WantsQuit() const { return m_request_quit; }
};

} // namespace

int main(int, char **)
{
#if NBEDITOR
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD))
    {
        std::printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    const float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    const SDL_WindowFlags window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    char versionString[128];
    std::snprintf(versionString, sizeof(versionString), "NeoBAE Editor v0.00 (%s)", _VERSION);
    SDL_Window *window = SDL_CreateWindow(versionString, static_cast<int>(1280 * main_scale), static_cast<int>(820 * main_scale), window_flags);
    if (!window)
    {
        std::printf("SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer)
    {
        std::printf("SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    // Window stays hidden until after engine Init() succeeds.

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;

    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    NbEditorApp app;
    app.SetMainWindow(window);
    if (!app.Init())
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                                 "NeoBAE nbeditor",
                                 app.GetStatus(),
                                 nullptr);
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_ShowWindow(window);

    bool done = false;
    while (!done)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT)
            {
                done = true;
            }
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window))
            {
                done = true;
            }
        }

        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)
        {
            SDL_Delay(10);
            continue;
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        app.DrawUI();
        if (app.WantsQuit())
        {
            done = true;
        }

        ImGui::Render();
        SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(renderer, 18, 22, 28, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    app.Shutdown();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
#else
    return 0;
#endif
}
