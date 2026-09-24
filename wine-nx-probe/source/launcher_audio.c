#include "launcher_audio.h"
#include <SDL.h>
#include <math.h>
#include <stdlib.h>

#define SAMPLE_RATE 48000
#define MAX_FRAMES 4800

struct launcher_audio
{
    SDL_AudioDeviceID device;
    Sint16 samples[3][MAX_FRAMES];
    int frames[3], position[3];
    Uint32 last_move;
};

static void mix_audio( void *opaque, Uint8 *stream, int bytes )
{
    struct launcher_audio *audio = opaque;
    Sint16 *out = (Sint16 *)stream;
    int count = bytes / (2 * sizeof(*out));

    SDL_memset( stream, 0, bytes );
    for (int i = 0; i < count; i++)
    {
        int sample = 0;
        for (int sound = 0; sound < 3; sound++)
            if (audio->position[sound] < audio->frames[sound])
                sample += audio->samples[sound][audio->position[sound]++];
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        out[i * 2] = out[i * 2 + 1] = sample;
    }
}

static void make_sound( struct launcher_audio *audio, int sound, int frames, double frequency, int peak )
{
    int maximum = 0;

    for (int i = 0; i < frames; i++)
    {
        double t = (double)i / SAMPLE_RATE, progress = (double)i / (frames - 1);
        double attack = fmin( t / 0.004, 1.0 );
        double envelope = attack * attack * exp( -5.0 * progress ) * (1.0 - progress);
        double phase = 6.283185307179586 * frequency * t;
        double value = sin( phase ) + 0.22 * sin( phase * 1.5 );
        int sample = (int)(value * envelope * 16384);
        audio->samples[sound][i] = sample;
        if (abs( sample ) > maximum) maximum = abs( sample );
    }
    if (maximum)
        for (int i = 0; i < frames; i++)
            audio->samples[sound][i] = audio->samples[sound][i] * peak / maximum;
    audio->position[sound] = audio->frames[sound] = frames;
}

struct launcher_audio *launcher_audio_open(void)
{
    SDL_AudioSpec want = {0};
    struct launcher_audio *audio;

    if (SDL_InitSubSystem( SDL_INIT_AUDIO )) return NULL;
    if (!(audio = calloc( 1, sizeof(*audio) ))) goto failed;
    make_sound( audio, LAUNCHER_SOUND_MOVE, 1680, 820, 11000 );
    make_sound( audio, LAUNCHER_SOUND_ACCEPT, 3840, 660, 18000 );
    make_sound( audio, LAUNCHER_SOUND_BACK, 3360, 440, 14500 );
    want.freq = SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = mix_audio;
    want.userdata = audio;
    if (!(audio->device = SDL_OpenAudioDevice( NULL, 0, &want, NULL, 0 )))
    {
        free( audio );
        goto failed;
    }
    SDL_PauseAudioDevice( audio->device, 0 );
    return audio;
failed:
    SDL_QuitSubSystem( SDL_INIT_AUDIO );
    return NULL;
}

void launcher_audio_close( struct launcher_audio *audio )
{
    if (!audio) return;
    SDL_CloseAudioDevice( audio->device );
    SDL_QuitSubSystem( SDL_INIT_AUDIO );
    free( audio );
}

void launcher_audio_play( struct launcher_audio *audio, enum launcher_sound sound )
{
    Uint32 now = SDL_GetTicks();

    if (!audio || (unsigned int)sound > LAUNCHER_SOUND_BACK) return;
    if (sound == LAUNCHER_SOUND_MOVE)
    {
        if (audio->last_move && now - audio->last_move < 80) return;
        audio->last_move = now;
    }
    SDL_LockAudioDevice( audio->device );
    if (sound == LAUNCHER_SOUND_MOVE && (audio->position[LAUNCHER_SOUND_ACCEPT] < audio->frames[LAUNCHER_SOUND_ACCEPT] ||
                                       audio->position[LAUNCHER_SOUND_BACK] < audio->frames[LAUNCHER_SOUND_BACK]))
    {
        SDL_UnlockAudioDevice( audio->device );
        return;
    }
    if (audio->position[sound] == audio->frames[sound]) audio->position[sound] = 0;
    SDL_UnlockAudioDevice( audio->device );
}
