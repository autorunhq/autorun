#ifndef CONTROLS_TEST_PNG_H
#define CONTROLS_TEST_PNG_H

#include <stddef.h>
#include <stdint.h>

#define PNG_IMAGE_VERSION 1
#define PNG_FORMAT_BGRA 2
#define PNG_IMAGE_SIZE(img) ((size_t)(img).width * (size_t)(img).height * 4u)

typedef struct {
    void *opaque;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t flags;
    uint32_t colormap_entries;
    uint32_t warning_or_error;
    char message[64];
} png_image;

static inline int png_image_begin_read_from_memory( png_image *image, const void *memory, size_t size )
{
    (void)image; (void)memory; (void)size;
    return 0;
}

static inline int png_image_finish_read( png_image *image, const void *background, void *buffer,
                                         int32_t row_stride, void *colormap )
{
    (void)image; (void)background; (void)buffer; (void)row_stride; (void)colormap;
    return 0;
}

static inline void png_image_free( png_image *image )
{
    (void)image;
}

#endif
