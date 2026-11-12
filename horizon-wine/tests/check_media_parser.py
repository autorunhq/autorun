#!/usr/bin/env python3
"""Host check of the media parser (PORTING.md, stage 1): source/media_unix.c
plays files, the way DirectShow and the Windows Media reader ask it to.

    check_media_parser.py [--keep] [NAME...]

The parser is built for this machine against FFmpeg configured with the lines
of cmake/Media.cmake and nothing else, so a file plays here only if the
console's build has its demuxer and its decoders. A second FFmpeg build, with
the ffmpeg program, encoders and muxers added, writes the files and decodes
each of them itself; tests/media_parser.c holds what the parser hands out
against that: every picture and every sample, the times, what follows a move
in the file, a stop position, one stream alone, a stream switched off, every
picture format and other sound formats, compressed packets, both call tables,
short reads, files cut short, files that are refused, and a disconnect while a
read is not answered. Address and undefined-behaviour sanitizers watch all of
it.

It needs the FFmpeg checkout (sh horizon-wine/tools/bootstrap-wma.sh) and a
widl (python3 horizon-wine/tests/media_host.py --widl, or Wine's PE build named
in WINE_NX_PE_BUILD_DIR, whose headers are then used). The first run builds
FFmpeg twice, a few minutes; later runs take seconds. --keep leaves the files
in place and says where; names run those files only.

What it cannot say: FFmpeg here is plain C on this machine's processor, the
console's has its ARM64 routines; and nothing of Windows is in it, neither
quartz nor wmvcore nor a program.
"""
import os
import platform
import random
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import media_host  # noqa: E402

horizon = Path(__file__).resolve().parents[1]
root = horizon.parent
source = horizon / 'source/media_unix.c'
test = horizon / 'tests/media_parser.c'

SECONDS = 2
TONE = 440

# Windows has names for these, so a program that asks for compressed output
# gets their packets; the samples of the first five kinds of PCM go out as
# they are in the file.
VIDEO_TYPES = {'wmv1': 'wmv1', 'wmv2': 'wmv2', 'mpeg1video': 'mpeg1', 'mpeg2video': 'mpeg1', 'cinepak': 'cinepak'}
AUDIO_TYPES = {'wmav1': 'wma1', 'wmav2': 'wma2', 'mp2': 'mpeg1-2', 'aac': 'aac'}
PLAIN_PCM = {'pcm_u8', 'pcm_s16le', 'pcm_s24le', 'pcm_s32le', 'pcm_f32le'}

# Decoders of the runtime's build no file here holds: FFmpeg has no encoder for
# them. A decoder that is neither played nor named here fails the check, so
# the list of cmake/Media.cmake cannot grow past it unnoticed.
NO_ENCODER = {'h264', 'wmapro', 'wmalossless', 'wmv3', 'vc1', 'msmpeg4v1', 'indeo3', 'indeo4', 'indeo5',
              'mp1', 'mp3', 'gsm_ms'}


class Clip:
    def __init__(self, name, demuxer, video=None, audio=None, extra=(), **expect):
        self.name, self.demuxer, self.extra, self.expect = name, demuxer, list(extra), expect
        self.video = video      # (codec, arguments, width, height, frames a second)
        self.audio = audio      # (codec, rate, channels)


def picture(codec, *arguments, size=(160, 120), rate=15):
    return (codec, list(arguments), size[0], size[1], rate)


CLIPS = [
    # Windows Media, both generations of each codec.
    Clip('intro.wmv', 'asf', picture('wmv2', '-b:v', '200k'), ('wmav2', 22050, 2)),
    Clip('old.asf', 'asf', picture('wmv1', '-b:v', '200k'), ('wmav1', 22050, 2)),
    # AVI with the codecs of its time. The decoders of Cinepak, RLE, Video 1
    # and uncompressed AVI give RGB or a palette, and the last three mark no
    # frame as a key frame.
    Clip('cut.avi', 'avi', picture('msmpeg4v3', '-b:v', '200k'), ('adpcm_ms', 22050, 2)),
    Clip('v2.avi', 'avi', picture('msmpeg4v2', '-b:v', '200k'), ('pcm_u8', 22050, 1)),
    Clip('cinepak.avi', 'avi', picture('cinepak'), ('pcm_s16le', 22050, 1), close=1),
    Clip('rle.avi', 'avi', picture('msrle', '-pix_fmt', 'pal8'), close=1, nokeys=1),
    Clip('cram.avi', 'avi', picture('msvideo1'), close=1, nokeys=1),
    # Every frame of uncompressed AVI is a key frame, and FFmpeg rounds the
    # place to move to to the nearest frame: half the time the one after.
    Clip('plain.avi', 'avi', picture('rawvideo', '-pix_fmt', 'bgr24', size=(80, 60)), ('pcm_s16le', 22050, 2),
         close=1, late=1),
    # A width that is no multiple of four: the rows of what comes out are.
    Clip('xvid.avi', 'avi', picture('mpeg4', '-vtag', 'XVID', '-b:v', '200k', size=(162, 118)), ('mp2', 22050, 2)),
    Clip('h263.avi', 'avi', picture('h263', '-b:v', '200k', size=(176, 144)), ('adpcm_ima_wav', 22050, 1)),
    # MPEG program streams start half a second in and have no index: a move
    # lands on the key frame after the place, up to twelve frames later. A
    # time is written once a packet of the file, not once a picture, so
    # some packets of pictures have none.
    Clip('movie.mpg', 'mpegps', picture('mpeg1video', '-b:v', '300k', rate=25), ('mp2', 22050, 2), ['-f', 'mpeg'],
         shift=1, late=12, untimed=1),
    Clip('dvd.vob', 'mpegps', picture('mpeg2video', '-b:v', '300k', rate=25), ('mp2', 22050, 2), ['-f', 'vob'],
         shift=1, late=12, untimed=1),
    # Bare MPEG video says nothing of its length, and has no times at all.
    Clip('bare.m2v', 'mpegvideo', picture('mpeg2video', '-b:v', '300k', rate=25), None, ['-f', 'mpeg2video'],
         duration=0, length=SECONDS, late=12, untimed=1),
    # MP4 names its streams' languages. (A stream's name is read as well,
    # but FFmpeg writes none its own demuxer reads back as one.) After a move
    # to the start the packet the AAC decoder warms up on is not read again:
    # two packets of sound come out faded in.
    Clip('clip.mp4', 'mov', picture('mpeg4', '-b:v', '200k'), ('aac', 22050, 2),
         ['-metadata:s:v:0', 'language=eng', '-metadata:s:a:0', 'language=fra'],
         vlang='eng', alang='fra', prime=2048),
    Clip('song.mp2', 'mp3', None, ('mp2', 22050, 2)),
    Clip('voice.wav', 'wav', None, ('adpcm_ima_wav', 22050, 1)),
    Clip('music.wav', 'wav', None, ('pcm_s16le', 22050, 2)),
    Clip('wide.wav', 'wav', None, ('pcm_s24le', 22050, 2)),
    Clip('wider.wav', 'wav', None, ('pcm_s32le', 22050, 2)),
    Clip('float.wav', 'wav', None, ('pcm_f32le', 22050, 2)),
    Clip('law.wav', 'wav', None, ('pcm_alaw', 8000, 1)),
    # AU and AIFF hold big-endian samples.
    Clip('tone.au', 'au', None, ('pcm_mulaw', 8000, 1)),
    Clip('tone16.au', 'au', None, ('pcm_s16be', 22050, 2)),
    Clip('tone32.au', 'au', None, ('pcm_s32be', 22050, 1)),
    Clip('float.au', 'au', None, ('pcm_f32be', 22050, 1)),
    Clip('double.au', 'au', None, ('pcm_f64be', 22050, 1)),
    Clip('tone.aiff', 'aiff', None, ('pcm_s16be', 22050, 2)),
    Clip('tone8.aiff', 'aiff', None, ('pcm_s8', 22050, 1)),
    Clip('tone24.aiff', 'aiff', None, ('pcm_s24be', 22050, 2)),
]

# Motion JPEG is not in the runtime's build: its stream is left out and the
# sound beside it plays; alone, the file is refused.
MIXED = Clip('mixed.avi', 'avi', picture('mjpeg'), ('pcm_s16le', 22050, 2))
MOTION = Clip('motion.avi', 'avi', picture('mjpeg'))


def fail(text):
    sys.exit('check_media_parser: ' + text)


def run(command, **options):
    return subprocess.run([str(part) for part in command], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, **options)


def must(command, what):
    result = run(command)
    if result.returncode:
        fail('%s:\n%s' % (what, result.stdout[-3000:]))
    return result.stdout


def generated_headers(compiler, flags, folder):
    """The headers widl makes of include/*.idl, as far as media_unix.c reaches them."""
    built = os.environ.get('WINE_NX_PE_BUILD_DIR')
    if built and (Path(built) / 'include/mfobjects.h').exists():
        return Path(built) / 'include'
    widl = media_host.widl()
    out = folder / 'include'
    out.mkdir()
    for _ in range(64):
        # -MG lists a header that is nowhere as a file of this folder.
        listed = must([compiler, '-E', '-M', '-MG'] + flags + ['-I', out, test], 'reading the includes')
        missing = sorted({name for name in re.findall(r'(?<![\w/.-])([\w-]+)\.h\b(?!/)', listed)
                          if not (out / (name + '.h')).exists() and (root / 'include' / (name + '.idl')).exists()
                          and not (root / 'include' / (name + '.h')).exists()})
        if not missing:
            return out
        for name in missing:
            must([widl, '-h', '-o', out / (name + '.h'), '-m64', '--nostdinc', '-I', root / 'include',
                  '-D__WINESRC__', root / 'include' / (name + '.idl')],
                 '%s could not make %s.h (python3 horizon-wine/tests/media_host.py --widl builds one that can)'
                 % (widl, name))
    fail('the includes of media_unix.c never settle')


def build_test(ship, folder):
    compiler = os.environ.get('WINE_NX_HOST_CC', 'cc')
    flags = ['-std=gnu11', '-D__WINESRC__', '-DWINE_UNIX_LIB', '-D_WIN64', '-I', root / 'include',
             '-I', ship / 'include']
    headers = generated_headers(compiler, flags, folder)
    program = folder / 'media_parser'
    # The warnings left off are ones the runtime's own build of media_unix.c
    # leaves off; the last two lines are the same ones by clang's names.
    result = run([compiler] + flags + [
        '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-sign-compare', '-Wno-missing-field-initializers',
        '-Wno-override-init', '-Wno-initializer-overrides', '-Wno-unknown-warning-option',
        '-O1', '-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
        '-I', headers, test, '-L', ship / 'lib', '-lavformat', '-lavcodec', '-lswscale', '-lswresample', '-lavutil',
        '-lm', '-lpthread', '-o', program])
    if result.returncode:
        fail('tests/media_parser.c did not build:\n' + result.stdout[-4000:])
    mine = [line for line in result.stdout.splitlines() if 'warning' in line and 'media_parser.c' in line]
    if mine:
        fail('tests/media_parser.c builds with warnings:\n' + '\n'.join(mine[:10]))
    return program


class Maker:
    def __init__(self, tool, folder):
        self.ffmpeg, self.folder = tool / 'bin/ffmpeg', folder
        self.quiet = [self.ffmpeg, '-hide_banner', '-loglevel', 'error', '-y']

    def make(self, clip):
        """Write the file, and what the writing build makes of it itself."""
        path = self.folder / clip.name
        command = list(self.quiet)
        if clip.video:
            command += ['-f', 'lavfi', '-i', 'testsrc=size=%dx%d:rate=%d' % clip.video[2:5]]
        if clip.audio:
            command += ['-f', 'lavfi', '-i', 'sine=frequency=%d:sample_rate=%d' % (TONE, clip.audio[1])]
        command += ['-t', str(SECONDS)]
        if clip.video:
            command += ['-c:v', clip.video[0]] + clip.video[1]
        if clip.audio:
            command += ['-c:a', clip.audio[0], '-ac', str(clip.audio[2])]
        must(command + clip.extra + [path], 'writing ' + clip.name)
        return path

    def packets(self, path, stream, out):
        listed = must(self.quiet + ['-i', path, '-map', '0:%s:0' % stream, '-c', 'copy', '-f', 'framecrc', '-'],
                      'listing the packets of ' + path.name)
        lines = [line.split(',') for line in listed.splitlines() if line and not line.startswith('#')]
        out.write_text(''.join('%d %s\n' % (int(fields[4]), fields[5].strip()[2:]) for fields in lines))
        return len(lines)

    def expectations(self, clip, path, decoded=('v', 'a')):
        """The arguments that tell tests/media_parser.c what the file is."""
        video = clip.video if 'v' in decoded else None
        audio = clip.audio if 'a' in decoded else None
        told = {'streams': bool(video) + bool(audio), 'duration': SECONDS}
        packets = {}
        if video:
            codec, _, width, height, rate = video
            yuv = Path(str(path) + '.yuv')
            # Every decoded frame once, as planes; where the decoder gives
            # RGB, turned into planes with the filter the parser uses.
            must(self.quiet + ['-i', path, '-map', '0:v:0', '-fps_mode', 'passthrough', '-sws_flags', 'bilinear',
                               '-pix_fmt', 'yuv420p', '-f', 'rawvideo', yuv], 'decoding the pictures of ' + clip.name)
            frame = width * height + 2 * ((width + 1) // 2) * ((height + 1) // 2)
            frames, rest = divmod(yuv.stat().st_size, frame)
            if rest or frames != SECONDS * rate:
                fail('%s decodes to %d pictures and %d bytes, %d were written' % (clip.name, frames, rest, SECONDS * rate))
            told.update(width=width, height=height, fps_n=rate, fps_d=1, frames=frames, yuv=yuv,
                        vtype=VIDEO_TYPES.get(codec, 'none'))
            if codec in VIDEO_TYPES:
                packets['vcrc'] = Path(str(path) + '.video.txt')
                self.packets(path, 'v', packets['vcrc'])
        if audio:
            codec, rate, channels = audio
            pcm = Path(str(path) + '.pcm')
            must(self.quiet + ['-i', path, '-map', '0:a:0', '-c:a', 'pcm_s16le', '-f', 's16le', pcm],
                 'decoding the sound of ' + clip.name)
            seconds = pcm.stat().st_size / (2 * channels * rate)
            if abs(seconds - SECONDS) > 0.2:
                fail('%s decodes to %.2f s of sound, %d s were written' % (clip.name, seconds, SECONDS))
            told.update(rate=rate, channels=channels, pcm=pcm, atype=AUDIO_TYPES.get(codec, 'none'))
            if codec in AUDIO_TYPES or codec in PLAIN_PCM:
                packets['acrc'] = Path(str(path) + '.sound.txt')
                self.packets(path, 'a', packets['acrc'])
                packets['samples'] = int(codec in PLAIN_PCM)
        told.update(clip.expect)
        return told, packets


class Runner:
    def __init__(self, program):
        self.program, self.runs, self.notes = program, 0, []

    def __call__(self, mode, path, told=None, expect=(0,), **more):
        told = dict(told or {}, **more)
        result = run([self.program, mode, path] + ['%s=%s' % item for item in told.items()], timeout=300)
        self.runs += 1
        if result.returncode not in expect:
            extra = ' '.join('%s=%s' % item for item in more.items())
            said = result.stdout.strip()
            # A sanitizer's report starts with what it found.
            found = re.search(r'^.*(ERROR: \w+Sanitizer|runtime error:)', said, re.M)
            said = said[found.start():][:3000] if found else said[-3000:]
            fail('%s %s %s: %s' % (mode, path.name, extra, said or 'ended with %d' % result.returncode))
        return result.stdout


def main():
    arguments = sys.argv[1:]
    keep = '--keep' in arguments
    names = [name for name in arguments if name != '--keep']
    unknown = set(names) - {clip.name for clip in CLIPS}
    if unknown:
        fail('no such file: ' + ', '.join(sorted(unknown)))

    shipped = media_host.shipped_flags()
    listed = {kind: {name for flag in shipped if flag.startswith('--enable-%s=' % kind)
                     for name in flag.split('=')[1].split(',')} for kind in ('demuxer', 'decoder')}
    played = {'demuxer': {clip.demuxer for clip in CLIPS},
              'decoder': {part[0] for clip in CLIPS for part in (clip.video, clip.audio) if part}}
    unplayed = listed['demuxer'] - played['demuxer']
    if unplayed:
        fail('no file here is read by the demuxer the runtime builds: ' + ', '.join(sorted(unplayed)))
    unplayed = listed['decoder'] - played['decoder'] - NO_ENCODER
    if unplayed:
        fail('no file here holds what the runtime builds a decoder for: ' + ', '.join(sorted(unplayed)))
    stray = played['decoder'] - listed['decoder'] - PLAIN_PCM
    if stray:
        fail('files here hold what the runtime builds no decoder for: ' + ', '.join(sorted(stray)))

    ship, tool = media_host.folders()
    folder = Path(tempfile.mkdtemp(prefix='run-', dir=str(media_host.host_folder())))
    try:
        check = Runner(build_test(ship, folder))
        maker = Maker(tool, folder)
        low = True
        held = set()
        for clip in CLIPS:
            if names and clip.name not in names:
                continue
            path = maker.make(clip)
            told, packets = maker.expectations(clip, path)
            check('play', path, told)
            check('packets', path, told, **packets)
            check('rebase', path, told)
            # Reads that come back shorter than asked.
            check('play', path, told, chunk=1000)
            # The table a 32-bit program calls.
            if low:
                for mode, more in (('play', {}), ('packets', packets)):
                    if 'not run' in check(mode, path, told, wow64=1, expect=(0, 77), **more):
                        low = False
                        if platform.system() == 'Linux' and platform.machine() in ('x86_64', 'AMD64'):
                            fail('no memory below 4 GB on a machine that has it')
                        check.notes.append('the 32-bit table was not run: no memory below 4 GB on this machine')
                        break
            # The file ends early, or a read fails, at six tenths: what is
            # there plays. MP4 keeps its index at the end and is refused.
            size = path.stat().st_size
            cut = folder / ('cut-' + clip.name)
            cut.write_bytes(path.read_bytes()[:size * 6 // 10])
            if clip.demuxer == 'mov':
                check('broken', cut)
                check('broken', path, fail_at=size * 6 // 10)
            else:
                check('play', cut, told, partial=1)
                check('play', path, told, partial=1, fail_at=size * 6 // 10)
            # The program leaves while a read is not answered: the first,
            # one while the file is opened, one while it plays.
            for hold in (0, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89):
                said = check('abort', path, hold=hold, chunk=1500)
                held.add((said.split(':')[1].strip().startswith('no answer'), ', opened' in said))
            
        if not names:
            # Not a media file at all.
            noise = random.Random(96)
            for name, data in (('noise.avi', bytes(noise.getrandbits(8) for _ in range(65536))),
                               ('text.wmv', b'This is not a film.\n' * 400), ('one.wav', b'R'),
                               ('riff.avi', b'RIFF\xff\xff\xff\x7fAVI LIST' + bytes(4096))):
                (folder / name).write_bytes(data)
                check('broken', folder / name)
            # A stream there is no decoder for.
            path = maker.make(MIXED)
            told, _ = maker.expectations(MIXED, path, decoded=('a',))
            check('play', path, told)
            check('broken', maker.make(MOTION))
        if not names:
            # What no file can be told to hold.
            result = run([check.program, 'units'], timeout=300)
            check.runs += 1
            if result.returncode:
                fail('units: ' + (result.stdout.strip()[-3000:] or 'ended with %d' % result.returncode))
        if not names and not {(True, False), (True, True)} <= held:
            fail('no read was left unanswered both while a file was opened and while it played: %s' % sorted(held))
        left = sorted(listed['decoder'] & NO_ENCODER)
        print('check_media_parser: %d files, %d runs: every picture, sample and packet is the one the file holds'
              % (len(names) or len(CLIPS) + 6, check.runs))
        print('  demuxers played: ' + ', '.join(sorted(listed['demuxer'])))
        print('  decoders not played, FFmpeg has no encoder for them: ' + ', '.join(left))
        for note in check.notes:
            print('  ' + note)
        if keep:
            print('  kept in ' + str(folder))
    finally:
        if not keep:
            shutil.rmtree(folder, ignore_errors=True)


if __name__ == '__main__':
    main()
