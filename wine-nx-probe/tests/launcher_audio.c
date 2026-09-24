#include <assert.h>
#include "../source/launcher_audio.c"

int main(void)
{
    struct launcher_audio *audio;
    Sint16 output[1024];
    int sound, i;
    SDL_setenv( "SDL_AUDIODRIVER", "dummy", 1 );
    audio = launcher_audio_open();
    assert( audio && SDL_GetAudioDeviceStatus( audio->device ) == SDL_AUDIO_PLAYING );
    SDL_PauseAudioDevice( audio->device, 1 );
    for (sound = 0; sound < 3; sound++)
    {
        static const int peaks[] = {11000, 18000, 14500};
        unsigned int nonzero = 0;
        int maximum = 0;
        for (i = 0; i < audio->frames[sound]; i++)
        {
            int level = abs( audio->samples[sound][i] );
            assert( level <= peaks[sound] );
            if (level > maximum) maximum = level;
            nonzero += audio->samples[sound][i] != 0;
        }
        assert( maximum == peaks[sound] );
        assert( nonzero > 500 );
        assert( !audio->samples[sound][0] );
        assert( !audio->samples[sound][audio->frames[sound] - 1] );
        launcher_audio_play( audio, sound );
        for (int offset = 0; offset < MAX_FRAMES; offset += 512)
        {
            mix_audio( audio, (Uint8 *)output, sizeof(output) );
            for (i = 0; i < 512; i++)
            {
                int expected = offset + i < audio->frames[sound] ? audio->samples[sound][offset + i] : 0;
                assert( output[i * 2] == expected && output[i * 2 + 1] == expected );
            }
            if (!offset)
            {
                for (i = 0; i < 500; i++) launcher_audio_play( audio, sound );
                assert( audio->position[sound] == 512 );
            }
        }
        assert( audio->position[sound] == audio->frames[sound] );
    }
    mix_audio( audio, (Uint8 *)output, sizeof(output) );
    for (i = 0; i < 1024; i++) assert( !output[i] );
    for (sound = 0; sound < 3; sound++) audio->position[sound] = 0;
    mix_audio( audio, (Uint8 *)output, sizeof(output) );
    for (i = 0; i < 512; i++)
    {
        int expected = 0;
        for (sound = 0; sound < 3; sound++) expected += audio->samples[sound][i];
        if (expected > 32767) expected = 32767;
        if (expected < -32768) expected = -32768;
        assert( output[i * 2] == expected && output[i * 2 + 1] == expected );
    }
    for (sound = 0; sound < 3; sound++)
    {
        audio->position[sound] = 0;
        audio->samples[sound][0] = 20000;
        audio->samples[sound][1] = -20000;
    }
    mix_audio( audio, (Uint8 *)output, sizeof(output) );
    assert( output[0] == 32767 && output[1] == 32767 );
    assert( output[2] == -32768 && output[3] == -32768 );
    SDL_PauseAudioDevice( audio->device, 0 );
    launcher_audio_play( audio, LAUNCHER_SOUND_ACCEPT );
    SDL_Delay( 200 );
    assert( SDL_GetAudioDeviceStatus( audio->device ) == SDL_AUDIO_PLAYING );
    SDL_LockAudioDevice( audio->device );
    assert( audio->position[LAUNCHER_SOUND_ACCEPT] == audio->frames[LAUNCHER_SOUND_ACCEPT] );
    SDL_UnlockAudioDevice( audio->device );
    launcher_audio_close( audio );
    assert( !SDL_WasInit( SDL_INIT_AUDIO ) );
    audio = launcher_audio_open();
    assert( audio );
    launcher_audio_play( audio, LAUNCHER_SOUND_ACCEPT );
    launcher_audio_close( audio );
    launcher_audio_close( NULL );
    launcher_audio_play( NULL, LAUNCHER_SOUND_BACK );
    return 0;
}
