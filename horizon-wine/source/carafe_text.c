#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include <switch.h>

#include "carafe_settings.h"
#include "carafe_text.h"

#define FACE_SLOTS 12
#define RUN_LIMIT 512
#define RUN_MAX 64

typedef enum {
    FaceZhHans,
    FaceZhHansExt,
    FaceZhHant,
    FaceKo,
    FaceStandard,
    FaceCount,
} Face;

typedef struct {
    TTF_Font* font;
    Face face;
    int size;
    bool bold;
} FaceSlot;

typedef struct {
    TTF_Font* font;
    const char* start;
    size_t length;
} Run;

static const PlSharedFontType FACE_TYPES[FaceCount] = {
    PlSharedFontType_ChineseSimplified,
    PlSharedFontType_ExtChineseSimplified,
    PlSharedFontType_ChineseTraditional,
    PlSharedFontType_KO,
    PlSharedFontType_Standard,
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static FaceSlot g_slots[FACE_SLOTS];
static int g_next;
static bool g_service_tried;
static bool g_service_ready;
static bool g_face_tried[FaceCount];
static PlFontData g_face_data[FaceCount];
static bool g_face_loaded[FaceCount];

static bool faceLoaded(Face face) {
    if (!g_service_tried) {
        g_service_tried = true;
        g_service_ready = R_SUCCEEDED(plInitialize(PlServiceType_User));
    }
    if (!g_service_ready) {
        return false;
    }
    if (!g_face_tried[face]) {
        g_face_tried[face] = true;
        g_face_loaded[face] = R_SUCCEEDED(plGetSharedFontByType(&g_face_data[face], FACE_TYPES[face]))
            && g_face_data[face].address != NULL && g_face_data[face].size > 0;
    }
    return g_face_loaded[face];
}

static TTF_Font* faceAt(Face face, int size, bool bold) {
    for (int i = 0; i < FACE_SLOTS; i++) {
        FaceSlot* slot = &g_slots[i];
        if (slot->font != NULL && slot->face == face && slot->size == size && slot->bold == bold) {
            return slot->font;
        }
    }
    if (!faceLoaded(face)) {
        return NULL;
    }
    FaceSlot* slot = &g_slots[g_next++ % FACE_SLOTS];
    if (slot->font != NULL) {
        TTF_CloseFont(slot->font);
    }
    slot->font = TTF_OpenFontRW(SDL_RWFromConstMem(g_face_data[face].address, (int)g_face_data[face].size), 1, size);
    slot->face = face;
    slot->size = size;
    slot->bold = bold;
    if (slot->font != NULL && bold) {
        TTF_SetFontStyle(slot->font, TTF_STYLE_BOLD);
    }
    return slot->font;
}

static int preferredFaces(Face* faces) {
    switch (carafeLanguage()) {
    case CarafeLanguageZhHans:
        faces[0] = FaceZhHans;
        faces[1] = FaceZhHansExt;
        return 2;
    case CarafeLanguageZhHant:
        faces[0] = FaceZhHant;
        return 1;
    case CarafeLanguageKo:
        faces[0] = FaceKo;
        return 1;
    default:
        return 0;
    }
}

static bool cjk(uint32_t point) {
    return (point >= 0x1100 && point <= 0x11FF) || (point >= 0x2E80 && point <= 0x9FFF)
        || (point >= 0xA960 && point <= 0xA97F) || (point >= 0xAC00 && point <= 0xD7FF)
        || (point >= 0xF900 && point <= 0xFAFF) || (point >= 0xFE30 && point <= 0xFE4F)
        || (point >= 0xFF00 && point <= 0xFFEF) || (point >= 0x20000 && point <= 0x3FFFF);
}

static bool provides(TTF_Font* font, uint32_t point) {
    return font != NULL && TTF_GlyphIsProvided32(font, point) != 0;
}

static TTF_Font* fontFor(TTF_Font* primary, int size, bool bold, uint32_t point) {
    Face preferred[2];
    int count = preferredFaces(preferred);
    if (cjk(point)) {
        for (int i = 0; i < count; i++) {
            TTF_Font* font = faceAt(preferred[i], size, bold);
            if (provides(font, point)) {
                return font;
            }
        }
    }
    if (provides(primary, point)) {
        return primary;
    }
    for (int face = 0; face < FaceCount; face++) {
        TTF_Font* font = faceAt((Face)face, size, bold);
        if (provides(font, point)) {
            return font;
        }
    }
    return primary;
}

static size_t decode(const char* text, uint32_t* point) {
    const unsigned char* at = (const unsigned char*)text;
    size_t length = at[0] >= 0xF0 ? 4 : at[0] >= 0xE0 ? 3 : at[0] >= 0xC0 ? 2 : 1;
    uint32_t value = length == 1 ? at[0] : length == 2 ? at[0] & 0x1Fu : length == 3 ? at[0] & 0x0Fu : at[0] & 0x07u;
    for (size_t i = 1; i < length; i++) {
        if ((at[i] & 0xC0u) != 0x80u) {
            *point = 0xFFFD;
            return i;
        }
        value = value << 6 | (at[i] & 0x3Fu);
    }
    *point = value;
    return length;
}

static bool needsRuns(const char* text) {
    for (const unsigned char* at = (const unsigned char*)text; *at != '\0'; at++) {
        if (*at >= 0xE0) {
            return true;
        }
    }
    return false;
}

static int splitRuns(TTF_Font* primary, int size, bool bold, const char* text, Run* runs) {
    int count = 0;
    for (const char* at = text; *at != '\0';) {
        uint32_t point = 0;
        size_t length = decode(at, &point);
        TTF_Font* font = fontFor(primary, size, bold, point);
        if (count > 0 && runs[count - 1].font == font) {
            runs[count - 1].length += length;
        } else if (count < RUN_MAX) {
            runs[count++] = (Run) { font, at, length };
        } else {
            runs[count - 1].length += length;
        }
        at += length;
    }
    return count;
}

static int runWidth(const Run* run) {
    char part[RUN_LIMIT];
    size_t length = run->length < sizeof(part) - 1 ? run->length : sizeof(part) - 1;
    memcpy(part, run->start, length);
    part[length] = '\0';
    int w = 0;
    int h = 0;
    return TTF_SizeUTF8(run->font, part, &w, &h) == 0 ? w : 0;
}

int carafeTextMeasure(TTF_Font* primary, int size, bool bold, const char* text) {
    int w = 0;
    int h = 0;
    if (primary == NULL || text[0] == '\0') {
        return 0;
    }
    if (!needsRuns(text)) {
        return TTF_SizeUTF8(primary, text, &w, &h) == 0 ? w : 0;
    }
    pthread_mutex_lock(&g_lock);
    Run runs[RUN_MAX];
    int count = splitRuns(primary, size, bold, text, runs);
    for (int i = 0; i < count; i++) {
        w += runWidth(&runs[i]);
    }
    pthread_mutex_unlock(&g_lock);
    return w;
}

static SDL_Surface* renderOne(TTF_Font* font, const char* text) {
    SDL_Color white = { 255, 255, 255, 255 };
    SDL_Surface* drawn = TTF_RenderUTF8_Blended(font, text, white);
    if (drawn == NULL) {
        return NULL;
    }
    SDL_Surface* surface = SDL_ConvertSurfaceFormat(drawn, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(drawn);
    return surface;
}

static void blitAlpha(SDL_Surface* into, const SDL_Surface* from, int left, int top) {
    for (int y = 0; y < from->h; y++) {
        int ty = top + y;
        if (ty < 0 || ty >= into->h) {
            continue;
        }
        const uint32_t* source = (const uint32_t*)((const uint8_t*)from->pixels + y * from->pitch);
        uint32_t* target = (uint32_t*)((uint8_t*)into->pixels + ty * into->pitch);
        for (int x = 0; x < from->w; x++) {
            int tx = left + x;
            if (tx < 0 || tx >= into->w) {
                continue;
            }
            uint32_t alpha = source[x] >> 24;
            if (alpha > target[tx] >> 24) {
                target[tx] = alpha << 24 | 0xFFFFFFu;
            }
        }
    }
}

SDL_Surface* carafeTextRender(TTF_Font* primary, int size, bool bold, const char* text) {
    if (primary == NULL || text[0] == '\0') {
        return NULL;
    }
    if (!needsRuns(text)) {
        return renderOne(primary, text);
    }
    pthread_mutex_lock(&g_lock);
    Run runs[RUN_MAX];
    int count = splitRuns(primary, size, bold, text, runs);
    int widths[RUN_MAX];
    int total = 0;
    for (int i = 0; i < count; i++) {
        widths[i] = runWidth(&runs[i]);
        total += widths[i];
    }
    SDL_Surface* out = total > 0
        ? SDL_CreateRGBSurfaceWithFormat(0, total, TTF_FontHeight(primary), 32, SDL_PIXELFORMAT_ARGB8888)
        : NULL;
    if (out != NULL) {
        SDL_FillRect(out, NULL, 0);
        int left = 0;
        for (int i = 0; i < count; i++) {
            char part[RUN_LIMIT];
            size_t length = runs[i].length < sizeof(part) - 1 ? runs[i].length : sizeof(part) - 1;
            memcpy(part, runs[i].start, length);
            part[length] = '\0';
            SDL_Surface* piece = renderOne(runs[i].font, part);
            if (piece != NULL) {
                blitAlpha(out, piece, left, TTF_FontAscent(primary) - TTF_FontAscent(runs[i].font));
                SDL_FreeSurface(piece);
            }
            left += widths[i];
        }
    }
    pthread_mutex_unlock(&g_lock);
    return out;
}

void carafeTextRelease(void) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < FACE_SLOTS; i++) {
        if (g_slots[i].font != NULL) {
            TTF_CloseFont(g_slots[i].font);
            g_slots[i].font = NULL;
        }
    }
    g_next = 0;
    pthread_mutex_unlock(&g_lock);
}
