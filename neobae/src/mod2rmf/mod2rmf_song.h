#ifndef MOD2RMF_SONG_H
#define MOD2RMF_SONG_H
#include "mod2rmf_common.h"

int mod2rmf_ensure_loop_cc_resets(ModSongModel *song);
int mod2rmf_ensure_loop_pitch_bend_resets(ModSongModel *song);

void mod2rmf_song_model_init(ModSongModel *song);
void mod2rmf_song_model_dispose(ModSongModel *song);
int mod2rmf_song_model_append_note(ModSongModel *song,
                                  uint16_t sourceChannel,
                                  uint32_t startTick,
                                  uint32_t durationTicks,
                                  unsigned char note,
                                  unsigned char velocity,
                                  unsigned char program);
int mod2rmf_song_model_append_cc_event(ModSongModel *song,
                                      uint16_t sourceChannel,
                                      uint32_t tick,
                                      unsigned char cc,
                                      unsigned char value,
                                      unsigned char program);
int mod2rmf_song_model_append_pitch_bend(ModSongModel *song,
                                        uint16_t sourceChannel,
                                        uint32_t tick,
                                        uint16_t value,
                                        unsigned char program);
int mod2rmf_song_model_append_tempo_change(ModSongModel *song,
                                          uint32_t tick,
                                          uint32_t bpm);
void mod2rmf_analyze_channel_usage(const ModSongModel *song,
                                  ChannelProfile profiles[],
                                  uint32_t maxChannels);

void mod2rmf_channel_profile_cleanup(ChannelProfile *profiles, uint32_t count);
int mod2rmf_channel_profile_add_range(ChannelProfile *p, uint32_t start, uint32_t end);
void mod2rmf_channel_profile_add_program(ChannelProfile *p, uint8_t program);
int mod2rmf_spread_channels_by_program(ModSongModel *song, XBOOL isMod,
                                      uint8_t stereoSep);

#endif /* MOD2RMF_SONG_H */