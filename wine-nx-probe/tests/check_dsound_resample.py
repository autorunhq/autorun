#!/usr/bin/env python3
"""Check mixer accuracy, chunk boundaries, filters and silent advancement."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
mixer = (root / 'dlls/dsound/mixer.c').read_text()


def function(name):
    start = mixer.index(name)
    brace = mixer.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (mixer[end] == '{') - (mixer[end] == '}')
        end += 1
    return mixer[start:end] + '\n'


fixture = r'''
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char BYTE;
typedef uint32_t DWORD, UINT;
typedef uint64_t UINT64;
typedef int64_t LONG64;
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define DSBPLAY_LOOPING 1
#define STATE_STOPPING 2
#define DSBFREQUENCY_MAX 200000
#define FREQ_ADJUST_SHIFT 32
#define DECLSPEC_ALIGN(n) __attribute__((aligned(n)))
#define FIXED_0_32_TO_FLOAT(x) ((int)((x) >> 1) * (1.0f / (1ll << 31)))
#include "fir.h"
typedef struct { DWORD nChannels, nSamplesPerSec, nBlockAlign; } WAVEFORMATEX;
typedef struct {
    WAVEFORMATEX *pwfx;
    float *cp_buffer;
    DWORD cp_buffer_len, filter_buffer_len;
    BYTE *filter_buffer;
} DirectSoundDevice;
typedef struct IDirectSoundBufferImpl IDirectSoundBufferImpl;
typedef void (*bitsgetfunc)(const IDirectSoundBufferImpl *, BYTE *, BYTE *, unsigned, DWORD);
typedef void (*bitsputfunc)(const IDirectSoundBufferImpl *, DWORD, DWORD, float);
struct IDirectSoundBufferImpl {
    WAVEFORMATEX *pwfx;
    DirectSoundDevice *device;
    struct { BYTE *memory; } *buffer;
    DWORD buflen, sec_mixpos, playflags, mix_channels, state;
    DWORD freqAdjustNum, freqAdjustDen, input_delay, num_filters;
    float firgain, *input_tail;
    BOOL input_tail_valid;
    bitsgetfunc get;
    bitsputfunc put;
};
static int audible = 1;
static BOOL secondarybuffer_is_audible(IDirectSoundBufferImpl *dsb) { (void)dsb; return audible; }
static float *output;
static void put_float(const IDirectSoundBufferImpl *dsb, DWORD pos, DWORD channel, float value)
{ (void)dsb; output[pos / sizeof(float) + channel] = value; }
'''

filter_fixture = r'''
static void filter_samples(const IDirectSoundBufferImpl *dsb, BYTE *buffer, DWORD buflen,
        DWORD mixpos, DWORD count, BYTE *dst)
{
    UINT i;
    get_samples(dsb, buffer, buflen, mixpos, 0, count, dsb->pwfx->nBlockAlign, dst, get_raw);
    for (i = 0; i < count * dsb->pwfx->nChannels; i++) ((float *)dst)[i] *= 0.5f;
}
'''

tests = r'''
#define PI 3.14159265358979323846
struct stream {
    IDirectSoundBufferImpl dsb;
    DirectSoundDevice device;
    WAVEFORMATEX in, out;
    struct { BYTE *memory; } storage;
};
static void open_stream(struct stream *s, UINT rate, UINT channels, int filtered)
{
    UINT i, c;
    memset(s, 0, sizeof(*s));
    s->in = (WAVEFORMATEX){channels, rate, channels * sizeof(float)};
    s->out = (WAVEFORMATEX){channels, 48000, channels * sizeof(float)};
    s->device.pwfx = &s->out;
    s->storage.memory = malloc(rate * channels * sizeof(float));
    for (i = 0; i < rate; i++)
        for (c = 0; c < channels; c++)
            ((float *)s->storage.memory)[i * channels + c] = 0.5 * sin(2 * PI * (c+1)*1000 * i/rate);
    s->dsb.pwfx = &s->in;
    s->dsb.device = &s->device;
    s->dsb.buffer = (void *)&s->storage;
    s->dsb.buflen = rate * s->in.nBlockAlign;
    s->dsb.playflags = DSBPLAY_LOOPING;
    s->dsb.mix_channels = channels;
    s->dsb.freqAdjustNum = rate;
    s->dsb.freqAdjustDen = 48000;
    s->dsb.firgain = min(1.0f, 48000.0f/rate);
    s->dsb.input_delay = ((FIR_WIDTH-1)*DSBFREQUENCY_MAX+47999)/48000;
    s->dsb.input_tail = calloc(s->dsb.input_delay*channels, sizeof(float));
    s->dsb.get = getieee32;
    s->dsb.put = put_float;
    s->dsb.num_filters = filtered;
}
static void close_stream(struct stream *s)
{
    free(s->storage.memory); free(s->dsb.input_tail);
    free(s->device.cp_buffer); free(s->device.filter_buffer);
}
static void render(struct stream *s, float *dest, UINT count, DWORD *acc)
{
    output = dest;
    UINT advance = cp_fields_resample(&s->dsb, count, acc);
    s->dsb.sec_mixpos = (s->dsb.sec_mixpos + advance*s->in.nBlockAlign) % s->dsb.buflen;
}
static void check(UINT rate, UINT channels, int filtered)
{
    const UINT frames = 96000, chunks[] = {1, 17, 333, 480, 1024};
    struct stream a, b;
    DWORD acc_a = 0, acc_b = 0;
    UINT pos, i, c, chunk = 0;
    double worst = 0;
    float *x = calloc(frames*channels, sizeof(float));
    float *y = calloc(frames*channels, sizeof(float));
    open_stream(&a, rate, channels, filtered); open_stream(&b, rate, channels, filtered);
    for (pos = 0; pos < frames; pos += 480) render(&a, x+pos*channels, 480, &acc_a);
    for (pos = 0; pos < frames; pos += i) {
        i = min(chunks[chunk++ % 5], frames-pos);
        render(&b, y+pos*channels, i, &acc_b);
    }
    assert(acc_a == acc_b && a.dsb.sec_mixpos == b.dsb.sec_mixpos);
    for (i = 2048; i < frames; i++) for (c = 0; c < channels; c++) {
        double phase = (double)i*rate/48000 - a.dsb.input_delay
                + (double)(FIR_WIDTH/2-1)*DSBFREQUENCY_MAX/48000
                + max(0.0, (double)rate/48000-1);
        double ideal = (filtered ? 0.25 : 0.5)*sin(2*PI*(c+1)*1000*phase/rate);
        double error = fabs(x[i*channels+c]-ideal);
        if (error > worst) worst = error;
        assert(fabs(x[i*channels+c]-y[i*channels+c]) < 0.0001);
    }
    printf("%u Hz, %u channels, filter=%d: maximum error %.6f\n", rate, channels, filtered, worst);
    fflush(stdout);
    assert(worst < 0.006);
    audible = 0;
    render(&a, NULL, 480, &acc_a);
    assert(!a.dsb.input_tail_valid);
    audible = 1;
    close_stream(&a); close_stream(&b); free(x); free(y);
}
int main(void)
{
    check(22050, 1, 0); check(44100, 2, 0); check(32000, 2, 1); check(96000, 2, 0);
    puts("PASS: dsound resampling");
    return 0;
}
'''
source = fixture + ''.join(function(name) for name in (
    'static void getieee32(', 'static void get_raw(', 'static inline void get_samples('))
source += filter_fixture + ''.join(function(name) for name in (
    'static void downsample(', 'static void resample(', 'static UINT cp_fields_resample(')) + tests
with tempfile.TemporaryDirectory() as tmp:
    c_file, exe = Path(tmp) / 'resample.c', Path(tmp) / 'resample'
    c_file.write_text(source)
    subprocess.run(['cc', '-std=gnu99', '-O2', '-Wall', '-Wno-unused-parameter', '-Werror',
                    '-U__SSE__', '-fsanitize=address,undefined', '-I', str(root / 'dlls/dsound'),
                    '-o', str(exe), str(c_file), '-lm'], check=True)
    subprocess.run([str(exe)], check=True)
