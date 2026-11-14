#ifndef CARAFE_TEXT_H
#define CARAFE_TEXT_H

#include <stdbool.h>

#include <SDL.h>
#include <SDL_ttf.h>

int carafeTextMeasure(TTF_Font* primary, int size, bool bold, const char* text);
SDL_Surface* carafeTextRender(TTF_Font* primary, int size, bool bold, const char* text);
void carafeTextRelease(void);

#endif
