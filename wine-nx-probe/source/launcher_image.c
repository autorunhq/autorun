#include "launcher_image.h"
#include <SDL.h>
#include <png.h>
#include <turbojpeg.h>
#include <sys/stat.h>

#define IMAGE_MAX_BYTES (16u * 1024u * 1024u)
#define IMAGE_MAX_SIDE 2048

int launcher_image_decode( const void *data, size_t size, struct launcher_icon *icon )
{
    struct launcher_icon decoded = {0};
    int ok = 0;
    if (!data || size < 8 || size > IMAGE_MAX_BYTES) return 0;
    if (!png_sig_cmp( data, 0, 8 ))
    {
        png_image png = {0};
        png.version = PNG_IMAGE_VERSION;
        if (png_image_begin_read_from_memory( &png, data, size ) &&
            png.width && png.height && png.width <= IMAGE_MAX_SIDE && png.height <= IMAGE_MAX_SIDE)
        {
            png.format = PNG_FORMAT_RGBA;
            decoded.width = png.width;
            decoded.height = png.height;
            decoded.size = PNG_IMAGE_SIZE( png );
            if ((decoded.data = malloc( decoded.size )))
                ok = png_image_finish_read( &png, NULL, decoded.data, 0, NULL );
        }
        png_image_free( &png );
    }
    else if (((const unsigned char *)data)[0] == 0xff && ((const unsigned char *)data)[1] == 0xd8)
    {
        tjhandle decoder = tjInitDecompress();
        int sampling, space;
        if (!decoder) return 0;
        if (!tjDecompressHeader3( decoder, data, size, &decoded.width, &decoded.height, &sampling, &space ) &&
            decoded.width > 0 && decoded.height > 0 && decoded.width <= IMAGE_MAX_SIDE && decoded.height <= IMAGE_MAX_SIDE)
        {
            decoded.size = (size_t)decoded.width * decoded.height * 4;
            if ((decoded.data = malloc( decoded.size )))
                ok = !tjDecompress2( decoder, data, size, decoded.data, decoded.width, 0, decoded.height,
                                    TJPF_RGBA, TJFLAG_FASTDCT | TJFLAG_LIMITSCANS );
        }
        tjDestroy( decoder );
    }
    if (!ok) { free( decoded.data ); return 0; }
    decoded.kind = LAUNCHER_ICON_RGBA;
    *icon = decoded;
    return 1;
}

int launcher_image_load( const char *path, struct launcher_icon *icon )
{
    struct stat st;
    unsigned char *data;
    FILE *file;
    int ok;
    if (!path || !path[0] || stat( path, &st ) || !S_ISREG( st.st_mode ) ||
        st.st_size <= 0 || st.st_size > IMAGE_MAX_BYTES) return 0;
    if (!(data = malloc( st.st_size ))) return 0;
    if (!(file = fopen( path, "rb" ))) { free( data ); return 0; }
    ok = fread( data, 1, st.st_size, file ) == (size_t)st.st_size;
    fclose( file );
    if (ok) ok = launcher_image_decode( data, st.st_size, icon );
    free( data );
    return ok;
}

int launcher_image_square( struct launcher_icon *icon )
{
    SDL_Surface *source, *square;
    SDL_Rect rect;
    unsigned char *data;
    int side, ok;
    if (icon->kind != LAUNCHER_ICON_RGBA || !icon->data || icon->width <= 0 || icon->height <= 0) return 0;
    if (!launcher_icon_fit( icon, 256 )) return 0;
    side = icon->width > icon->height ? icon->width : icon->height;
    rect.w = icon->width * 256 / side;
    rect.h = icon->height * 256 / side;
    if (!rect.w) rect.w = 1;
    if (!rect.h) rect.h = 1;
    rect.x = (256 - rect.w) / 2;
    rect.y = (256 - rect.h) / 2;
    if (!(data = malloc( 256 * 256 * 4 ))) return 0;
    source = SDL_CreateRGBSurfaceWithFormatFrom( icon->data, icon->width, icon->height, 32,
                                                icon->width * 4, SDL_PIXELFORMAT_RGBA32 );
    square = SDL_CreateRGBSurfaceWithFormatFrom( data, 256, 256, 32, 256 * 4, SDL_PIXELFORMAT_RGBA32 );
    ok = source && square;
    if (ok)
    {
        SDL_FillRect( square, NULL, SDL_MapRGBA( square->format, 22, 27, 30, 255 ) );
        SDL_SetSurfaceBlendMode( source, SDL_BLENDMODE_BLEND );
        ok = !SDL_BlitScaled( source, NULL, square, &rect );
    }
    SDL_FreeSurface( source );
    SDL_FreeSurface( square );
    if (!ok) { free( data ); return 0; }
    launcher_icon_free( icon );
    *icon = (struct launcher_icon){ LAUNCHER_ICON_RGBA, 256, 256, data, 256 * 256 * 4 };
    return 1;
}

int launcher_image_jpeg( const struct launcher_icon *icon, unsigned char **data, size_t *size )
{
    tjhandle encoder;
    unsigned char *jpeg = NULL;
    unsigned long bytes = 0;
    int ok;
    *data = NULL;
    *size = 0;
    if (icon->kind != LAUNCHER_ICON_RGBA || icon->width != 256 || icon->height != 256 || !icon->data) return 0;
    if (!(encoder = tjInitCompress())) return 0;
    ok = !tjCompress2( encoder, icon->data, 256, 0, 256, TJPF_RGBA, &jpeg, &bytes, TJSAMP_420, 90, 0 ) &&
         bytes && bytes <= 0x20000;
    if (ok && (*data = malloc( bytes ))) { memcpy( *data, jpeg, bytes ); *size = bytes; }
    else ok = 0;
    tjFree( jpeg );
    tjDestroy( encoder );
    return ok;
}
