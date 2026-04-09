#ifndef MOD2RMF_RMFCREAT_H
#define MOD2RMF_RMFCREAT_H
#include "mod2rmf_resampler.h"
#include "mod2rmf_common.h"

typedef struct {
    void *sourceData;
    size_t sourceSize;
    ModRawSample *rawSamples;
    uint32_t rawSampleCount;
    uint32_t moduleBaseRateHz;
    bool isMod;
    bool isIt;

    BAERmfEditorDocument *document;
    uint16_t *channelToTrackIndex;
    ChannelMap channelMap;
    Mod2RmfResamplerSettings resamplerSettings;
    bool forceOriginalSamples;
    double sampleGainDb;      /* global sample gain applied during source extraction */
    uint8_t rootShiftSemitones; /* virtual root-key downshift for extra low-note range */
    bool avoidMidiChannel10;   /* map around MIDI ch10 (index 9) when requested */
    uint8_t stereoSeparation;  /* 0=mono (center), 75=default, 100=hard L/R */
    uint8_t itV00CutRows;      /* 0 disables; default 6 */
} Mod2RmfConverter;


#ifdef __cplusplus
extern "C" {
#endif

BAEResult mod2rmf_load_module_to_document(BAERmfEditorDocument **doc, const char *sourcePath, bool useZmfContainer);
bool mod2rmf_path_is_it(const char *path);

#ifdef __cplusplus
}
#endif

int mod2rmf_load_source_data(Mod2RmfConverter *conv, const char *sourcePath);
int mod2rmf_setup_samples(Mod2RmfConverter *conv, const ModSongModel *song);
int mod2rmf_setup_document(Mod2RmfConverter *conv,
                          const ModSongModel *song,
                          const char *sourcePath);

int mod2rmf_save_document(Mod2RmfConverter *conv, const char *destPath);
Mod2RmfConverter *mod2rmf_converter_create(void);
void mod2rmf_converter_delete(Mod2RmfConverter *conv);
int mod2rmf_flush_active_note(ModSongModel *song,
                             uint16_t sourceChannel,
                             ActiveNote *note,
                             uint64_t endTickFP);
int mod2rmf_write_song_tempo_events(Mod2RmfConverter *conv, const ModSongModel *song);                             
int mod2rmf_write_song_cc_events(Mod2RmfConverter *conv, const ModSongModel *song);
int mod2rmf_write_song_pitch_bend_events(Mod2RmfConverter *conv, const ModSongModel *song);
int mod2rmf_add_programmed_note(Mod2RmfConverter *conv,
                               uint16_t trackIndex,
                               uint32_t startTick,
                               uint32_t durationTicks,
                               unsigned char note,
                               unsigned char velocity,
                               unsigned char program);

int mod2rmf_write_song_notes(Mod2RmfConverter *conv, const ModSongModel *song);
int mod2rmf_setup_instrument_ext(Mod2RmfConverter *conv, const ModSongModel *song, bool useZmfContainer);
int mod2rmf_setup_tracks(Mod2RmfConverter *conv, const ModSongModel *song, const ChannelMap *chMap);
void mod2rmf_build_midi_channel_aggregate(const ChannelProfile trackerProfiles[],
                                         const uint8_t trackerToMidi[],
                                         uint32_t trackerCount,
                                         uint8_t midiCh,
                                         ChannelProfile *agg);
int mod2rmf_build_song_model(Mod2RmfConverter *conv, ModSongModel *song);

#endif /* MOD2RMF_RMFCREAT_H */