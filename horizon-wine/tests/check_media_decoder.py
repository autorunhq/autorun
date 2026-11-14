#!/usr/bin/env python3
"""Host check of the decoder behind Media Foundation's H.264 and AAC decoders:
the wg_transform_* half of source/media_unix.c.

    check_media_decoder.py [--keep]

The decoder is built for this machine against FFmpeg configured with the lines
of cmake/Media.cmake, as tests/check_media_parser.py builds the parser, and
with the same two FFmpeg builds (see there for what the check needs). The
second build writes AAC files that hold a tone and decodes each of them itself.
tests/media_decoder.c pushes their packets and reads the sound back:

  - every sample is the one the other build decodes, as float and as 16 bits,
    with the channels left as they are or mixed, read into buffers of any size,
    with any queue length, through both call tables, from frames with a header
    and without, and from packets that hold two frames;
  - every buffer's time is the time of the sound in it, also at another rate
    than the file's;
  - packets that are not AAC cost the sound they held and nothing else;
  - a stream that changes its channels or its rate plays on in the output that
    was asked for.

What it cannot say: no picture goes through the H.264 half, since FFmpeg has
no encoder to write a file with (it is created and emptied, no more;
`media_decoder file FILE` plays one brought from elsewhere). The AAC files are
the plain kind FFmpeg's encoder writes: no HE-AAC. And as with the parser,
FFmpeg here is plain C on this machine's processor, and nothing of Windows is
in it.
"""
import platform
import shutil
import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_media_parser as parser_check  # noqa: E402
import media_host  # noqa: E402

parser_check.NAME = 'check_media_decoder'
fail, run, must = parser_check.fail, parser_check.run, parser_check.must

test = parser_check.horizon / 'tests/media_decoder.c'

SECONDS = 2
# name: rate, channels. At 320 kbit/s and without noise substitution the tone
# comes back to a thousandth of its height, which is what lets a sample out of
# place show (see check_tone in the test).
FILES = {'tone.m4a': (44100, 2), 'voice.m4a': (22050, 1), 'wide.m4a': (48000, 6), 'low.m4a': (32000, 2),
         'mono.m4a': (44100, 1)}
ENCODER = ['-c:a', 'aac', '-b:a', '320k', '-aac_pns', '0', '-aac_coder', 'fast']

# How a file is played and held against the other build's samples: the shape
# of the output, then the way it is asked for.
SHAPES = ((32, 2), (16, 2), (16, 1))
WAYS = ({}, {'capacity': 8}, {'capacity': 1000}, {'queue': 0}, {'queue': 7, 'capacity': 4096}, {'queue': 15},
        {'eager': 1}, {'eager': 1, 'queue': 15, 'capacity': 256}, {'adts': 1}, {'raw': 1}, {'join': 7},
        {'join': 7, 'adts': 1, 'queue': 3}, {'wow64': 1}, {'wow64': 1, 'adts': 1, 'capacity': 500})
# Other rates: the times and the tone are all there is to hold them against.
RATES = (8000, 11025, 22050, 44100, 48000, 96000)
# What stands in a packet's place (see play_file in the test).
KINDS = (0, 1, 2, 3, 4)


def data_of_wav(path):
    """The samples of a WAV file: the writing build has no other way to write float."""
    data = path.read_bytes()
    at = 12
    while at + 8 <= len(data):
        size = struct.unpack_from('<I', data, at + 4)[0]
        if data[at:at + 4] == b'data':
            return data[at + 8:at + 8 + size]
        at += 8 + size + (size & 1)
    fail('%s has no samples' % path.name)


class Runner:
    def __init__(self, program):
        self.program, self.runs, self.low = program, 0, True

    def __call__(self, mode, *paths, **told):
        if told.get('wow64') and not self.low:
            return ''
        result = run([self.program, mode] + list(paths) + ['%s=%s' % item for item in told.items()], timeout=300)
        self.runs += 1
        if result.returncode == 77 and told.get('wow64'):
            if platform.system() == 'Linux' and platform.machine() in ('x86_64', 'AMD64'):
                fail('no memory below 4 GB on a machine that has it')
            self.low = False
            return ''
        if result.returncode:
            said = result.stdout.strip()
            found = parser_check.re.search(r'^.*(ERROR: \w+Sanitizer|runtime error:)', said, parser_check.re.M)
            said = said[found.start():][:3000] if found else said[-3000:]
            fail('%s %s %s: %s' % (mode, ' '.join(path.name for path in paths),
                                   ' '.join('%s=%s' % item for item in told.items()),
                                   said or 'ended with %d' % result.returncode))
        return result.stdout


def main():
    keep = '--keep' in sys.argv[1:]
    if set(sys.argv[1:]) - {'--keep'}:
        fail('check_media_decoder.py [--keep]')
    shipped = media_host.shipped_flags()
    decoders = {name for flag in shipped if flag.startswith('--enable-decoder=') for name in flag.split('=')[1].split(',')}
    if not {'aac', 'h264'} <= decoders:
        fail('cmake/Media.cmake builds no decoder for: ' + ', '.join(sorted({'aac', 'h264'} - decoders)))

    ship, tool = media_host.folders()
    folder = Path(tempfile.mkdtemp(prefix='run-', dir=str(media_host.host_folder())))
    try:
        check = Runner(parser_check.build_test(ship, folder, test))
        quiet = [tool / 'bin/ffmpeg', '-hide_banner', '-loglevel', 'error', '-y']
        check('types')
        check('types', wow64=1)

        packets = {}
        for name, (rate, channels) in FILES.items():
            path = folder / name
            must(quiet + ['-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=%d' % rate, '-t', str(SECONDS)]
                 + ENCODER + ['-ac', str(channels), '-f', 'mp4', path], 'writing ' + name)
            # What the writing build decodes it to, in each shape.
            pcm = {}
            for bits, out in SHAPES:
                pcm[bits, out] = folder / ('%s.%d.%d.pcm' % (name, bits, out))
                if bits == 16:
                    must(quiet + ['-i', path, '-ac', str(out), '-c:a', 'pcm_s16le', '-f', 's16le', pcm[bits, out]],
                         'decoding ' + name)
                else:
                    wav = folder / (name + '.wav')
                    must(quiet + ['-i', path, '-ac', str(out), '-c:a', 'pcm_f32le', '-f', 'wav', wav],
                         'decoding ' + name)
                    pcm[bits, out].write_bytes(data_of_wav(wav))
                seconds = pcm[bits, out].stat().st_size / (bits // 8 * out * rate)
                if abs(seconds - SECONDS) > 0.05:
                    fail('%s decodes to %.3f s, %d s were written' % (name, seconds, SECONDS))
            for number, way in enumerate(WAYS):
                # Each way with one shape in turn, the first way with all of them.
                for bits, out in (SHAPES if not way else (SHAPES[number % len(SHAPES)],)):
                    said = check('play', path, pcm=pcm[bits, out], bits=bits, channels=out, **way)
            packets[name] = int(said.split()[0]) if said else int(check('play', path).split()[0])
            for number, out_rate in enumerate(RATES):
                if out_rate != rate:
                    check('play', path, **dict({'rate': out_rate, 'bits': (32, 16)[number % 2],
                                                'capacity': (65536, 300)[number % 2]}, **WAYS[5 + number]))

            # Damage: every kind at the first packets, in the middle and at the
            # end; then several packets in a row, more of them than the queue
            # holds; with headers; and at another rate.
            last = packets[name] - 1
            for kind in KINDS:
                for at in (0, 1, 5, last // 2, last - 1, last):
                    check('damaged', path, kind=kind, at=at)
            for number, (at, length) in enumerate(((0, 3), (5, 2), (9, 20), (last - 17, 18), (last // 2, 16))):
                kind = KINDS[number % len(KINDS)]
                check('damaged', path, kind=kind, at=at, run=length, queue=15)
                check('damaged', path, kind=kind, at=at, run=length, adts=1, eager=1, capacity=512)
                other = [out_rate for out_rate in RATES if out_rate != rate]
                check('damaged', path, kind=kind, at=at, run=length, rate=other[number], bits=16, wow64=number % 2)

        # A stream that changes. Without headers a channel is added; with them
        # channels and rates come and go.
        mono, tone, voice, wide, low = (folder / name for name in ('mono.m4a', 'tone.m4a', 'voice.m4a', 'wide.m4a',
                                                                    'low.m4a'))
        for way in ({}, {'rate': 48000}, {'bits': 16, 'capacity': 100, 'queue': 15}, {'raw': 1, 'rate': 22050},
                    {'wow64': 1, 'channels': 1}):
            check('change', mono, tone, **way)
        for way in ({}, {'rate': 48000, 'bits': 16}, {'rate': 22050, 'capacity': 1000, 'queue': 9},
                    {'rate': 8000, 'eager': 1, 'wow64': 1}, {'channels': 1, 'rate': 96000}):
            check('change', tone, low, wide, voice, mono, tone, adts=1, **way)
            check('change', wide, mono, wide, mono, adts=1, **way)
            check('change', voice, low, adts=1, **way)
            # The channels alone, at one rate.
            check('change', tone, mono, tone, adts=1, **way)

        print('check_media_decoder: %d files, %d runs: the sound is the file\'s, at its times; damage costs what it '
              'held; a stream that changes plays on' % (len(FILES), check.runs))
        if not check.low:
            print('  the 32-bit table was not run: no memory below 4 GB on this machine')
        print('  not played: H.264 (FFmpeg has no encoder for it), HE-AAC')
        if keep:
            print('  kept in ' + str(folder))
    finally:
        if not keep:
            shutil.rmtree(folder, ignore_errors=True)


if __name__ == '__main__':
    main()
