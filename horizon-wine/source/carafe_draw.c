#include <SDL.h>
#include <SDL_ttf.h>
#include <math.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <turbojpeg.h>

#include "carafe_draw.h"
#include "carafe_overlay.h"
#include "carafe_text.h"

#define FONT_SLOTS 16
#define FONT_LIMIT (4 * 1024 * 1024)
#define ICON_LIMIT (2 * 1024 * 1024)
#define LINE_LIMIT 512

typedef struct {
    TTF_Font* font;
    int size;
    CarafeWeight weight;
} FontSlot;

extern int wine_nx_osk_font(const void** data, size_t* size) __attribute__((weak));

static FontSlot g_fonts[FONT_SLOTS];
static int g_font_next;
static bool g_font_failed;
static void* g_font_data[CarafeWeightCount];
static size_t g_font_sizes[CarafeWeightCount];
static bool g_font_tried[CarafeWeightCount];
static uint32_t* g_background;
static int g_background_width;
static int g_background_height;
static unsigned int g_background_alpha;

static bool fontData(CarafeWeight weight, const void** data, size_t* bytes, bool* own) {
    static const char* const FILES[CarafeWeightCount] = {
        "/carafe/fonts/MPLUS1p-Regular.ttf",
        "/carafe/fonts/MPLUS1p-Bold.ttf",
        "/carafe/fonts/MPLUS1p-ExtraBold.ttf",
    };
    if (!g_font_tried[weight]) {
        g_font_tried[weight] = true;
        g_font_data[weight] = carafeOverlayReadRomfsFile(FILES[weight], FONT_LIMIT, &g_font_sizes[weight]);
    }
    if (g_font_data[weight] != NULL) {
        *data = g_font_data[weight];
        *bytes = g_font_sizes[weight];
        *own = true;
        return true;
    }
    *own = false;
    return wine_nx_osk_font != NULL && wine_nx_osk_font(data, bytes);
}

static TTF_Font* fontAt(int size, CarafeWeight weight) {
    for (int i = 0; i < FONT_SLOTS; i++) {
        if (g_fonts[i].font != NULL && g_fonts[i].size == size && g_fonts[i].weight == weight) {
            return g_fonts[i].font;
        }
    }
    if (g_font_failed) {
        return NULL;
    }
    if (!TTF_WasInit() && TTF_Init() != 0) {
        g_font_failed = true;
        return NULL;
    }
    const void* data = NULL;
    size_t bytes = 0;
    bool own = false;
    if (!fontData(weight, &data, &bytes, &own)) {
        g_font_failed = true;
        return NULL;
    }
    FontSlot* slot = &g_fonts[g_font_next++ % FONT_SLOTS];
    if (slot->font != NULL) {
        TTF_CloseFont(slot->font);
    }
    slot->font = TTF_OpenFontRW(SDL_RWFromConstMem(data, (int)bytes), 1, size);
    slot->size = size;
    slot->weight = weight;
    if (slot->font != NULL && !own && weight != CarafeRegular) {
        TTF_SetFontStyle(slot->font, TTF_STYLE_BOLD);
    }
    return slot->font;
}

void carafeDrawRelease(void) {
    carafeTextRelease();
    for (int i = 0; i < FONT_SLOTS; i++) {
        if (g_fonts[i].font != NULL) {
            TTF_CloseFont(g_fonts[i].font);
            g_fonts[i].font = NULL;
        }
    }
    for (int i = 0; i < CarafeWeightCount; i++) {
        free(g_font_data[i]);
        g_font_data[i] = NULL;
        g_font_sizes[i] = 0;
        g_font_tried[i] = false;
    }
    g_font_next = 0;
    free(g_background);
    g_background = NULL;
}

static uint32_t over(uint32_t under, uint32_t color, unsigned int alpha) {
    unsigned int below = under >> 24;
    if (alpha >= 255 || below == 0) {
        return (uint32_t)(alpha >= 255 ? 255 : alpha) << 24 | (color & 0xFFFFFFu);
    }
    unsigned int rest = below * (255 - alpha) / 255;
    unsigned int out = alpha + rest;
    if (out == 0) {
        return 0;
    }
    uint32_t r = ((color >> 16 & 0xFF) * alpha + (under >> 16 & 0xFF) * rest) / out;
    uint32_t g = ((color >> 8 & 0xFF) * alpha + (under >> 8 & 0xFF) * rest) / out;
    uint32_t b = ((color & 0xFF) * alpha + (under & 0xFF) * rest) / out;
    return (uint32_t)out << 24 | r << 16 | g << 8 | b;
}

static void blendPixel(CarafeImage* picture, int x, int y, uint32_t color, unsigned int alpha) {
    if (alpha == 0 || picture->pixels == NULL || x < 0 || y < 0 || x >= picture->width || y >= picture->height) {
        return;
    }
    uint32_t* pixel = &picture->pixels[y * picture->width + x];
    *pixel = over(*pixel, color, alpha);
}

static uint32_t mix(uint32_t from, uint32_t to, unsigned int amount) {
    uint32_t r = ((from >> 16 & 0xFF) * (255 - amount) + (to >> 16 & 0xFF) * amount) / 255;
    uint32_t g = ((from >> 8 & 0xFF) * (255 - amount) + (to >> 8 & 0xFF) * amount) / 255;
    uint32_t b = ((from & 0xFF) * (255 - amount) + (to & 0xFF) * amount) / 255;
    return r << 16 | g << 8 | b;
}

void carafeDrawBackground(CarafeImage* picture, unsigned int alpha) {
    size_t bytes = (size_t)picture->width * picture->height * 4;
    if (g_background != NULL && g_background_width == picture->width && g_background_height == picture->height
        && g_background_alpha == alpha) {
        memcpy(picture->pixels, g_background, bytes);
        return;
    }
    const float cx = picture->width * 0.78f;
    const float cy = picture->height * 0.46f;
    const float reach = sqrtf(cx * cx + (picture->height - cy) * (picture->height - cy));
    const uint32_t opacity = (uint32_t)(alpha > 255 ? 255 : alpha) << 24;
    for (int y = 0; y < picture->height; y++) {
        for (int x = 0; x < picture->width; x++) {
            float t = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy)) / reach;
            unsigned int inner = (unsigned int)(t / 0.45f * 255.0f);
            unsigned int outer = (unsigned int)(fminf(fmaxf((t - 0.45f) / 0.55f, 0.0f), 1.0f) * 255.0f);
            picture->pixels[y * picture->width + x] = opacity
                | (t < 0.45f ? mix(CARAFE_COLOR_CENTER, CARAFE_COLOR_MIDDLE, inner)
                             : mix(CARAFE_COLOR_MIDDLE, CARAFE_COLOR_EDGE, outer));
        }
    }
    free(g_background);
    g_background = malloc(bytes);
    if (g_background != NULL) {
        memcpy(g_background, picture->pixels, bytes);
        g_background_width = picture->width;
        g_background_height = picture->height;
        g_background_alpha = alpha;
    }
}

static float roundCoverage(float px, float py, int x, int y, int w, int h, int radius) {
    float cx = px < x + radius ? x + radius : px > x + w - radius ? x + w - radius : px;
    float cy = py < y + radius ? y + radius : py > y + h - radius ? y + h - radius : py;
    float d = sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy));
    float coverage = radius + 0.5f - d;
    return coverage <= 0.0f ? 0.0f : coverage >= 1.0f ? 1.0f : coverage;
}

void carafeDrawRound(CarafeImage* picture, int x, int y, int w, int h, int radius, uint32_t color, unsigned int alpha) {
    int y0 = y < 0 ? 0 : y;
    int y1 = y + h > picture->height ? picture->height : y + h;
    int x0 = x < 0 ? 0 : x;
    int x1 = x + w > picture->width ? picture->width : x + w;
    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) {
            float coverage = radius > 0 ? roundCoverage(px + 0.5f, py + 0.5f, x, y, w, h, radius) : 1.0f;
            blendPixel(picture, px, py, color, (unsigned int)(coverage * alpha));
        }
    }
}

void carafeDrawRoundStroke(CarafeImage* picture, int x, int y, int w, int h, int radius, int thickness, uint32_t color,
    unsigned int alpha) {
    int inner = radius > thickness ? radius - thickness : 0;
    for (int py = y; py < y + h; py++) {
        for (int px = x; px < x + w; px++) {
            float outer = roundCoverage(px + 0.5f, py + 0.5f, x, y, w, h, radius);
            float hole = roundCoverage(px + 0.5f, py + 0.5f, x + thickness, y + thickness, w - 2 * thickness,
                h - 2 * thickness, inner);
            float coverage = outer - hole;
            if (coverage > 0.0f) {
                blendPixel(picture, px, py, color, (unsigned int)(coverage * alpha));
            }
        }
    }
}

void carafeDrawCircle(CarafeImage* picture, int cx, int cy, int radius, uint32_t color, unsigned int alpha) {
    carafeDrawRound(picture, cx - radius, cy - radius, radius * 2, radius * 2, radius, color, alpha);
}

void carafeDrawRing(CarafeImage* picture, int cx, int cy, int radius, int thickness, uint32_t color, unsigned int alpha) {
    carafeDrawRoundStroke(picture, cx - radius, cy - radius, radius * 2, radius * 2, radius, thickness, color, alpha);
}

void carafeDrawFocusRing(CarafeImage* picture, int x, int y, int w, int h, int radius) {
    carafeDrawRound(picture, x - 7, y - 7, w + 14, h + 14, radius + 7, CARAFE_COLOR_FOCUS, 255);
    carafeDrawRound(picture, x - 4, y - 4, w + 8, h + 8, radius + 4, CARAFE_COLOR_GAP, 255);
    carafeDrawRound(picture, x, y, w, h, radius, CARAFE_COLOR_INK, 255);
}

int carafeTextWidth(const char* text, int size, CarafeWeight weight) {
    TTF_Font* font = fontAt(size, weight);
    if (font == NULL || text[0] == '\0') {
        return 0;
    }
    return carafeTextMeasure(font, size, weight != CarafeRegular, text);
}

void carafeDrawText(CarafeImage* picture, const char* text, int size, CarafeWeight weight, int x0, int top,
    uint32_t color) {
    TTF_Font* font = fontAt(size, weight);
    if (font == NULL || text[0] == '\0' || picture->pixels == NULL) {
        return;
    }
    SDL_Surface* surface = carafeTextRender(font, size, weight != CarafeRegular, text);
    if (surface == NULL) {
        return;
    }
    for (int y = 0; y < surface->h; y++) {
        const uint32_t* row = (const uint32_t*)((const uint8_t*)surface->pixels + y * surface->pitch);
        for (int x = 0; x < surface->w; x++) {
            blendPixel(picture, x0 + x, top + y, color, row[x] >> 24);
        }
    }
    SDL_FreeSurface(surface);
}

static int lineTop(int top, int size, int line_height, CarafeWeight weight) {
    TTF_Font* font = fontAt(size, weight);
    int height = font != NULL ? TTF_FontHeight(font) : size;
    return top + (line_height - height) / 2;
}

void carafeDrawLine(CarafeImage* picture, const char* line, const CarafeTextStyle* style, int x, int top, int width) {
    int left = style->centered ? x + (width - carafeTextWidth(line, style->size, style->weight)) / 2 : x;
    carafeDrawText(picture, line, style->size, style->weight, left,
        lineTop(top, style->size, style->line_height, style->weight), style->color);
}

static size_t charLength(const char* text) {
    unsigned char first = (unsigned char)text[0];
    size_t want = first >= 0xF0 ? 4 : first >= 0xE0 ? 3 : first >= 0xC0 ? 2 : 1;
    size_t len = 1;
    while (len < want && (unsigned char)text[len] >= 0x80 && (unsigned char)text[len] < 0xC0) {
        len++;
    }
    return len;
}

static int flush(CarafeImage* picture, char* line, size_t* used, const CarafeTextStyle* style, int x, int top,
    int width) {
    line[*used] = '\0';
    carafeDrawLine(picture, line, style, x, top, width);
    *used = 0;
    return top + style->line_height;
}

int carafeDrawWrapped(CarafeImage* picture, const char* text, const CarafeTextStyle* style, int x, int top, int width) {
    char line[LINE_LIMIT];
    char candidate[LINE_LIMIT];
    size_t used = 0;
    const char* at = text;
    while (*at != '\0') {
        bool space = *at == ' ';
        size_t length = charLength(at);
        const char* next = at + length;
        bool wide = (unsigned char)*at >= 0xE0;
        if (!space && !wide) {
            while (*next != '\0' && *next != ' ' && (unsigned char)*next < 0xE0) {
                next += charLength(next);
            }
            length = (size_t)(next - at);
        }
        if (space && used == 0) {
            at = next;
            continue;
        }
        int written = snprintf(candidate, sizeof(candidate), "%.*s%.*s", (int)used, line, (int)length, at);
        bool fits = written < (int)sizeof(candidate) && carafeTextWidth(candidate, style->size, style->weight) <= width;
        if (!fits && used > 0) {
            top = flush(picture, line, &used, style, x, top, width);
            if (space) {
                at = next;
                continue;
            }
            written = snprintf(candidate, sizeof(candidate), "%.*s", (int)length, at);
        }
        if (written >= (int)sizeof(candidate)) {
            written = (int)sizeof(candidate) - 1;
        }
        memcpy(line, candidate, (size_t)written);
        used = (size_t)written;
        at = next;
    }
    if (used > 0) {
        top = flush(picture, line, &used, style, x, top, width);
    }
    return top;
}

int carafeWrappedHeight(const char* text, const CarafeTextStyle* style, int width) {
    CarafeImage none = { NULL, 0, 0 };
    return carafeDrawWrapped(&none, text, style, 0, 0, width);
}

void carafeDrawPicture(CarafeImage* picture, const CarafeImage* source, int left, int top, int size, int radius) {
    if (source->pixels == NULL) {
        return;
    }
    for (int y = 0; y < size; y++) {
        const uint32_t* row = source->pixels + (size_t)(y * source->height / size) * source->width;
        for (int x = 0; x < size; x++) {
            float coverage = roundCoverage(x + 0.5f, y + 0.5f, 0, 0, size, size, radius);
            blendPixel(picture, left + x, top + y, 0xFF000000u | row[x * source->width / size],
                (unsigned int)(coverage * 255));
        }
    }
}

bool carafeLoadIcon(CarafeImage* icon) {
    size_t size = 0;
    unsigned char* data = carafeOverlayReadRomfsFile("/carafe/icon.jpg", ICON_LIMIT, &size);
    if (data == NULL) {
        return false;
    }
    static const unsigned char PNG_SIG[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    if (size >= 8 && memcmp(data, PNG_SIG, 8) == 0) {
        png_image png;
        memset(&png, 0, sizeof(png));
        png.version = PNG_IMAGE_VERSION;
        uint32_t* pixels = NULL;
        if (png_image_begin_read_from_memory(&png, data, size) && png.width > 0 && png.height > 0
            && png.width <= 1024 && png.height <= 1024) {
            png.format = PNG_FORMAT_BGRA;
            pixels = malloc(PNG_IMAGE_SIZE(png));
            if (pixels != NULL && png_image_finish_read(&png, NULL, pixels, 0, NULL)) {
                icon->pixels = pixels;
                icon->width = (int)png.width;
                icon->height = (int)png.height;
            } else {
                free(pixels);
            }
        }
        png_image_free(&png);
        free(data);
        return icon->pixels != NULL;
    }
    tjhandle decoder = tjInitDecompress();
    int width = 0;
    int height = 0;
    int sampling = 0;
    int space = 0;
    if (decoder != NULL && tjDecompressHeader3(decoder, data, (unsigned long)size, &width, &height, &sampling, &space) == 0
        && width > 0 && height > 0 && width <= 1024 && height <= 1024) {
        uint32_t* pixels = malloc((size_t)width * height * 4);
        if (pixels != NULL
            && tjDecompress2(decoder, data, (unsigned long)size, (unsigned char*)pixels, width, 0, height, TJPF_BGRA, 0) == 0) {
            icon->pixels = pixels;
            icon->width = width;
            icon->height = height;
        } else {
            free(pixels);
        }
    }
    if (decoder != NULL) {
        tjDestroy(decoder);
    }
    free(data);
    return icon->pixels != NULL;
}
