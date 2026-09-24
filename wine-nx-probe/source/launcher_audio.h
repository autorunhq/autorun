#ifndef LAUNCHER_AUDIO_H
#define LAUNCHER_AUDIO_H

struct launcher_audio;
enum launcher_sound { LAUNCHER_SOUND_MOVE, LAUNCHER_SOUND_ACCEPT, LAUNCHER_SOUND_BACK };

struct launcher_audio *launcher_audio_open(void);
void launcher_audio_close( struct launcher_audio *audio );
void launcher_audio_play( struct launcher_audio *audio, enum launcher_sound sound );

#endif
