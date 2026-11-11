#ifndef CONTROLS_TEST_SDL_TTF_H
#define CONTROLS_TEST_SDL_TTF_H

#include <string.h>
#include "SDL.h"

typedef struct { int dummy; } TTF_Font;
#define TTF_STYLE_BOLD 1

static inline int TTF_WasInit( void ) { return 1; }
static inline int TTF_Init( void ) { return 0; }
static inline TTF_Font *TTF_OpenFontRW( SDL_RWops *rw, int f, int sz )
{ (void)rw; (void)f; (void)sz; return NULL; }
static inline void TTF_CloseFont( TTF_Font *f ) { (void)f; }
static inline void TTF_SetFontStyle( TTF_Font *f, int s ) { (void)f; (void)s; }
static inline int TTF_GlyphIsProvided32( TTF_Font *f, uint32_t ch ) { (void)f; (void)ch; return 1; }
static inline int TTF_SizeUTF8( TTF_Font *f, const char *t, int *w, int *h )
{ (void)f; if (w) *w = t ? (int)strlen( t ) * 8 : 0; if (h) *h = 16; return 0; }
static inline SDL_Surface *TTF_RenderUTF8_Blended( TTF_Font *f, const char *t, SDL_Color c )
{ (void)f; (void)t; (void)c; return NULL; }
static inline int TTF_FontAscent( TTF_Font *f ) { (void)f; return 12; }
static inline int TTF_FontHeight( TTF_Font *f ) { (void)f; return 16; }

#endif
