#ifndef CARAFE_MENU_H
#define CARAFE_MENU_H

#include <stdbool.h>
#include <stdint.h>

#include "carafe_draw.h"
#include "carafe_settings.h"

typedef enum {
    CarafeMenuStay,
    CarafeMenuBack,
    CarafeMenuRestart,
} CarafeMenuAction;

bool carafeMenuVisible(void);
unsigned int carafeMenuGeneration(void);
unsigned int carafeMenuCopy(int screen_width, int screen_height, void* pixels, int stride, int rgba);

bool carafeMenuOpenError(CarafeFailure failure);
CarafeMenuAction carafeMenuInput(unsigned int held, int touching, int touch_x, int touch_y, uint64_t now_ns);
int carafeMenuHandleInput(unsigned int held, int stick_x, int stick_y, int touching, int touch_x, int touch_y,
    uint64_t now_ns);
void carafeMenuRender(CarafeImage* picture);
void carafeMenuClose(void);

#endif
