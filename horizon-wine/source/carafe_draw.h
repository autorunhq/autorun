#ifndef CARAFE_DRAW_H
#define CARAFE_DRAW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CARAFE_SCREEN_WIDTH 1280
#define CARAFE_SCREEN_HEIGHT 720

#define CARAFE_COLOR_INK 0xFFFFF5F8u
#define CARAFE_COLOR_TEXT 0xFFEBD6DDu
#define CARAFE_COLOR_HINT 0xFFC9A6B3u
#define CARAFE_COLOR_CHIP 0xFFF0A3B5u
#define CARAFE_COLOR_FOCUS 0xFFDB6E87u
#define CARAFE_COLOR_GAP 0xFF2A0D19u
#define CARAFE_COLOR_PRIMARY_INK 0xFF3B0F20u
#define CARAFE_COLOR_PANEL 0xFF12060Au
#define CARAFE_COLOR_LABEL 0xFFB08C99u
#define CARAFE_COLOR_VALUE 0xFFF4E4EAu
#define CARAFE_COLOR_ACCENT 0xFF8E2846u
#define CARAFE_COLOR_CENTER 0xFF5A1A30u
#define CARAFE_COLOR_MIDDLE 0xFF2A0D19u
#define CARAFE_COLOR_EDGE 0xFF140709u
#define CARAFE_COLOR_WHITE 0xFFFFFFFFu
#define CARAFE_COLOR_LIVE 0xFF8FDDB0u
#define CARAFE_COLOR_RESTART 0xFFE2BC55u
#define CARAFE_COLOR_RESTART_INK 0xFF2A1D00u
#define CARAFE_COLOR_DIM 0xFF6B4655u

typedef struct {
    uint32_t* pixels;
    int width;
    int height;
} CarafeImage;

typedef enum {
    CarafeRegular,
    CarafeBold,
    CarafeHeavy,
    CarafeWeightCount,
} CarafeWeight;

typedef struct {
    int size;
    CarafeWeight weight;
    int line_height;
    uint32_t color;
    bool centered;
} CarafeTextStyle;

void carafeDrawBackground(CarafeImage* picture, unsigned int alpha);
void carafeDrawRound(CarafeImage* picture, int x, int y, int w, int h, int radius, uint32_t color, unsigned int alpha);
void carafeDrawRoundStroke(CarafeImage* picture, int x, int y, int w, int h, int radius, int thickness, uint32_t color,
    unsigned int alpha);
void carafeDrawCircle(CarafeImage* picture, int cx, int cy, int radius, uint32_t color, unsigned int alpha);
void carafeDrawRing(CarafeImage* picture, int cx, int cy, int radius, int thickness, uint32_t color, unsigned int alpha);
int carafeTextWidth(const char* text, int size, CarafeWeight weight);
void carafeDrawText(CarafeImage* picture, const char* text, int size, CarafeWeight weight, int x, int top, uint32_t color);
void carafeDrawLine(CarafeImage* picture, const char* line, const CarafeTextStyle* style, int x, int top, int width);
int carafeDrawWrapped(CarafeImage* picture, const char* text, const CarafeTextStyle* style, int x, int top, int width);
int carafeWrappedHeight(const char* text, const CarafeTextStyle* style, int width);
void carafeDrawPicture(CarafeImage* picture, const CarafeImage* source, int left, int top, int size, int radius);
void carafeDrawFocusRing(CarafeImage* picture, int x, int y, int w, int h, int radius);
bool carafeLoadIcon(CarafeImage* icon);
void carafeDrawRelease(void);

#endif
