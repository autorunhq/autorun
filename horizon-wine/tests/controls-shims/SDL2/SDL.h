#ifndef CONTROLS_TEST_SDL_H
#define CONTROLS_TEST_SDL_H

#include <stddef.h>
#include <stdint.h>

typedef uint8_t Uint8;
typedef uint32_t Uint32;
typedef struct { uint8_t r, g, b, a; } SDL_Color;
typedef struct { int x, y, w, h; } SDL_Rect;
typedef struct { int w, h, pitch; void *pixels; } SDL_Surface;
typedef struct { int dummy; } SDL_RWops;

#define SDL_PIXELFORMAT_ARGB8888 0x16362004u
#define SDL_BLENDMODE_NONE 0

static inline SDL_RWops *SDL_RWFromConstMem( const void *m, int s ) { (void)m; (void)s; return NULL; }
static inline void SDL_FreeSurface( SDL_Surface *s ) { (void)s; }
static inline SDL_Surface *SDL_ConvertSurfaceFormat( SDL_Surface *s, Uint32 f, Uint32 fl )
{ (void)s; (void)f; (void)fl; return NULL; }
static inline SDL_Surface *SDL_CreateRGBSurfaceWithFormat( Uint32 fl, int w, int h, int d, Uint32 f )
{ (void)fl; (void)w; (void)h; (void)d; (void)f; return NULL; }
static inline int SDL_FillRect( SDL_Surface *d, const SDL_Rect *r, Uint32 c )
{ (void)d; (void)r; (void)c; return 0; }
static inline int SDL_SetSurfaceBlendMode( SDL_Surface *s, int m ) { (void)s; (void)m; return 0; }
static inline int SDL_BlitSurface( SDL_Surface *s, const SDL_Rect *sr, SDL_Surface *d, SDL_Rect *dr )
{ (void)s; (void)sr; (void)d; (void)dr; return 0; }

#endif
