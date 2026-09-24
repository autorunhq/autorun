#ifndef LAUNCHER_IMAGE_H
#define LAUNCHER_IMAGE_H

#include "launcher_pe.h"

int launcher_image_decode( const void *data, size_t size, struct launcher_icon *icon );
int launcher_image_load( const char *path, struct launcher_icon *icon );
int launcher_image_square( struct launcher_icon *icon );
int launcher_image_jpeg( const struct launcher_icon *icon, unsigned char **data, size_t *size );

#endif
