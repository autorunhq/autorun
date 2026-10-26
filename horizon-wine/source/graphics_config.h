#ifndef WINE_NX_GRAPHICS_CONFIG_H
#define WINE_NX_GRAPHICS_CONFIG_H

/* Screenshot pixels handed to wine_nx_screenshot_take (horizon-wine/source/
 * runtime.c): which bytes a pixel has and which way the rows run. */
enum wine_nx_screenshot_format
{
    WINE_NX_SHOT_RGBA8 = 0,            /* R,G,B,A bytes, first row on top */
    WINE_NX_SHOT_BGRA8 = 1,            /* B,G,R,A bytes, first row on top */
    WINE_NX_SHOT_RGBA8_BOTTOM_UP = 2,  /* R,G,B,A bytes, as glReadPixels gives them */
};

void wine_nx_graphics_configure(int frame_limit, int vsync);
void wine_nx_upscaling_configure(int mode, float sharpness);
void wine_nx_aspect_configure(int stretch43);

#endif
