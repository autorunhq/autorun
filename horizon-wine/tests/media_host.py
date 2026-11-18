#!/usr/bin/env python3
"""FFmpeg for the host, twice, and widl, for tests/check_media_parser.py.

    media_host.py            build what is missing of FFmpeg and print the two folders
    media_host.py --widl     fetch and build widl, and print where it is

"ship" is FFmpeg as the runtime has it: the configure lines of
cmake/Media.cmake, nothing else, so a file plays in the test only if the
console's build has its demuxer and decoders. "tool" is the same with the
ffmpeg program and the encoders and muxers that write the test's files.

Both are plain C (--disable-asm): the console's build has the ARM64 routines
this machine cannot run, and FFmpeg 7.1.5 without nasm keeps x86 routines
written inside the C files, whose chroma rows come out wrong when a picture
is halved in height (rgb24 to yuv420p gives 255 for grey).

The source is the checkout tools/bootstrap-wma.sh makes (vendor/ffmpeg) or
WINE_NX_FFMPEG_SOURCE. The builds go to WINE_NX_MEDIA_HOST, by default
/tmp/wine-nx-media-host, and are reused while the revision and the
configure lines stay the same.

widl makes the headers media_unix.c includes out of include/*.idl; the
runtime's build takes them from Wine's PE build. Wine's own widl needs flex
and bison to build, so this one is mingw-w64's copy of it, whose parser comes
ready made: fetched at one commit from github.com/mingw-w64/mingw-w64. A widl
named in WINE_NX_WIDL is used in its place, and one on the PATH when none was
built.
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

horizon = Path(__file__).resolve().parents[1]

# What writes the test's files. None of it is a demuxer or a decoder the
# runtime lacks, but for wrapped_avframe, which the ffmpeg program needs to
# read its own test picture. The mjpeg encoder writes the one stream the
# runtime must leave out: it has no decoder for it.
TOOL = [
    '--enable-ffmpeg',
    '--enable-avfilter', '--enable-avdevice', '--enable-indev=lavfi',
    '--enable-filter=testsrc,sine,scale,format,aformat,aresample,null,anull,trim,atrim',
    '--enable-decoder=wrapped_avframe',
    '--enable-encoder=wmv1,wmv2,wmav1,wmav2,msmpeg4v2,msmpeg4v3,mpeg1video,mpeg2video,mpeg4,h263,mp2,aac',
    '--enable-encoder=cinepak,msvideo1,msrle,rawvideo,mjpeg,adpcm_ms,adpcm_ima_wav',
    '--enable-encoder=pcm_u8,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,pcm_alaw,pcm_mulaw',
    '--enable-encoder=pcm_s8,pcm_s16be,pcm_s24be,pcm_s32be,pcm_f32be,pcm_f64be',
    '--enable-muxer=asf,avi,mov,mp4,mpeg1system,mpeg2vob,mpeg1video,mpeg2video,mp2,wav,aiff,au',
    '--enable-muxer=rawvideo,pcm_s16le,framecrc',
    '--enable-protocol=file,pipe',
]

# How both are built here.
FIXED = ['--disable-asm', '--disable-stripping', '--extra-cflags=-O1 -g']


def shipped_flags():
    text = (horizon / 'cmake/Media.cmake').read_text()
    command = text[text.index('CONFIGURE_COMMAND'):text.index('BUILD_COMMAND')]
    flags = [flag for flag in re.findall(r'--(?:enable|disable)-[\w-]+(?:=[\w,]+)?', command)
             if flag != '--enable-cross-compile']
    assert '--disable-everything' in flags and any(f.startswith('--enable-demuxer=asf') for f in flags), flags
    return flags


def source_folder():
    source = Path(os.environ.get('WINE_NX_FFMPEG_SOURCE', horizon / 'vendor/ffmpeg'))
    if not (source / 'configure').exists():
        sys.exit('no FFmpeg source at %s: run horizon-wine/tools/bootstrap-wma.sh' % source)
    return source


def build(name, flags, source, folder):
    revision = subprocess.run(['git', '-C', str(source), 'rev-parse', 'HEAD'], stdout=subprocess.PIPE,
                              text=True, check=True).stdout.strip()
    key = hashlib.sha256((revision + '\n' + '\n'.join(FIXED + flags)).encode()).hexdigest()
    install, mark = folder / name, folder / (name + '.key')
    if mark.exists() and mark.read_text() == key:
        return install
    work = folder / (name + '-build')
    work.mkdir(parents=True, exist_ok=True)
    log = folder / (name + '.log')
    with open(log, 'w') as out:
        for command in (['sh', str(source / 'configure'), '--prefix=' + str(install)] + FIXED + flags,
                        ['make', '-j%d' % (os.cpu_count() or 2)], ['make', 'install']):
            if subprocess.run(command, cwd=work, stdout=out, stderr=subprocess.STDOUT).returncode:
                sys.exit('FFmpeg for the host (%s) did not build: see %s' % (name, log))
    mark.write_text(key)
    return install


def host_folder():
    return Path(os.environ.get('WINE_NX_MEDIA_HOST', '/tmp/wine-nx-media-host'))


def folders():
    source = source_folder()
    folder = host_folder()
    shipped = shipped_flags()
    return build('ship', shipped, source, folder), build('tool', shipped + TOOL, source, folder)


WIDL_SOURCE = 'https://github.com/mingw-w64/mingw-w64'
WIDL_COMMIT = 'a215715f0b87c06f8eb11d3b499bfef66de47179'    # widl 11.20
WIDL_NAMES = ('widl', 'x86_64-w64-mingw32-widl', 'aarch64-w64-mingw32-widl', 'i686-w64-mingw32-widl')


def widl(make=False):
    """The widl to use; with make, fetch and build the one named above first."""
    named = os.environ.get('WINE_NX_WIDL')
    if named:
        return named
    folder = host_folder()
    built, mark = folder / 'widl-build' / 'widl', folder / 'widl.key'
    if built.exists() and mark.exists() and mark.read_text() == WIDL_COMMIT:
        return str(built)
    if not make:
        for name in WIDL_NAMES:
            if shutil.which(name):
                return name
        sys.exit('no widl here: name one in WINE_NX_WIDL, or fetch and build one with\n'
                 '    python3 horizon-wine/tests/media_host.py --widl')
    source, work, log = folder / 'widl-source', folder / 'widl-build', folder / 'widl.log'
    for old in (source, work):
        shutil.rmtree(old, ignore_errors=True)
    source.mkdir(parents=True)
    work.mkdir(parents=True)
    with open(log, 'w') as out:
        for command, where in (
                (['git', 'init', '-q'], source),
                (['git', 'sparse-checkout', 'set', 'mingw-w64-tools/widl'], source),
                (['git', 'fetch', '-q', '--depth', '1', '--filter=blob:none', WIDL_SOURCE, WIDL_COMMIT], source),
                (['git', 'checkout', '-q', 'FETCH_HEAD'], source),
                (['sh', str(source / 'mingw-w64-tools/widl/configure'), '--target=x86_64-w64-mingw32'], work),
                (['make', '-j%d' % (os.cpu_count() or 2)], work)):
            if subprocess.run(command, cwd=where, stdout=out, stderr=subprocess.STDOUT).returncode:
                sys.exit('widl did not build: see %s' % log)
    mark.write_text(WIDL_COMMIT)
    return str(built)


if __name__ == '__main__':
    if sys.argv[1:] == ['--widl']:
        print(widl(make=True))
    elif sys.argv[1:]:
        sys.exit(__doc__)
    else:
        for folder in folders():
            print(folder)
