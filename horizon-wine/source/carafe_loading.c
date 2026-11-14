#include <EGL/egl.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <SDL.h>
#include <SDL_ttf.h>
#include <png.h>
#include <pthread.h>
#include <turbojpeg.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "carafe_error.h"
#include "carafe_loading.h"
#include "carafe_menu.h"
#include "carafe_overlay.h"
#include "carafe_settings.h"
#include "carafe_text.h"
#include "osk.h"

#define STALL_SECONDS 60
#define VULKAN_FRAMES_SHOWN 30
#define PROBE_INTERVAL_NS 250000000ULL
#define PROBE_CONTENT_PIXELS 8
#define PROBE_CONFIRMATIONS 2
#define PROBE_BRIGHTNESS 60
#define FONT_SLOTS 8
#define GENERATION_BASE 0x40000000u
#define LOADER_STEP_MS 100
#define SPLASH_LOGO_LEFT 51
#define SPLASH_LOGO_TOP 15
#define SPLASH_MOVIE_RIGHT 1245
#define SPLASH_MOVIE_BOTTOM 670
#define SPLASH_SCALE_NUM 2
#define SPLASH_SCALE_DEN 3
#define SPLASH_LOGO_HEIGHT_NUM 5
#define SPLASH_LOGO_HEIGHT_DEN 8
#define SPLASH_LIMIT (256 * 1024)
#define SPLASH_MAX_SIDE 1024
#define MOVIE_MAX_FRAMES 256
#define MOVIE_DEFAULT_DELAY_MS 100
#define LZW_CODES 4096
#define LAYOUT_MARGIN 54
#define LAYOUT_LOGO_TOP 28
#define LAYOUT_LOGO_HEIGHT 37
#define ICON_LIMIT (2 * 1024 * 1024)
#define STEP_COUNT 3

#define COLOR_BACKGROUND 0xFF000000u
#define COLOR_TITLE 0xFFF1E9ECu
#define COLOR_TEXT 0xFFC9BCC2u
#define COLOR_DIM 0xFF8B7D84u
#define COLOR_WARN 0xFFE2BC55u
#define COLOR_STEP_DONE 0xFF8E2846u
#define COLOR_STEP_NOW 0xFFC0466Bu
#define COLOR_STEP_LATER 0xFF4A3D49u

typedef struct {
    const char* title;
    const char* steps[STEP_COUNT];
    const char* details;
    const char* stall;
    const char* hint;
    const char* keyboard;
    const char* settings;
} Strings;

static const Strings STRINGS_EN = {
    "Windows game",
    { "Starting the Windows environment", "Starting the game", "The game's first picture" },
    "%u s  ·  %llu MB read",
    "No progress for %u s: the game may have stopped responding.",
    "Press HOME and close the software to quit.",
    "keyboard",
    "settings",
};

static const Strings STRINGS_RU = {
    "Игра для Windows",
    { "Запуск среды Windows", "Запуск игры", "Первая картинка игры" },
    "%u с  ·  %llu МБ прочитано",
    "Нет прогресса %u с: игра, возможно, не отвечает.",
    "Нажмите HOME и закройте программу, чтобы выйти.",
    "клавиатура",
    "настройки",
};

static const Strings STRINGS_ZH_HANS = {
    "Windows 游戏",
    { "正在启动 Windows 环境", "正在启动游戏", "游戏的第一个画面" },
    "%u 秒  ·  已读取 %llu MB",
    "已有 %u 秒没有进展：游戏可能已停止响应。",
    "按下 HOME 按钮并关闭软件即可退出。",
    "键盘",
    "设置",
};

static const Strings STRINGS_JA = {
    "Windowsのゲーム",
    { "Windows環境を起動中", "ゲームを起動中", "ゲームの最初の画面" },
    "%u秒  ·  %llu MB読み込み済み",
    "%u秒間進んでいません。ゲームが応答していない可能性があります。",
    "やめるときは、HOMEボタンを押してソフトを終了してください。",
    "キーボード",
    "設定",
};

static const Strings STRINGS_KO = {
    "Windows 게임",
    { "Windows 환경을 시작하는 중", "게임을 시작하는 중", "게임의 첫 화면" },
    "%u초  ·  %llu MB 읽음",
    "%u초 동안 진행되지 않았습니다. 게임이 응답하지 않을 수 있습니다.",
    "종료하려면 HOME 버튼을 누르고 소프트웨어를 닫으세요.",
    "키보드",
    "설정",
};

static const Strings STRINGS_ZH_HANT = {
    "Windows 遊戲",
    { "正在啟動 Windows 環境", "正在啟動遊戲", "遊戲的第一個畫面" },
    "%u 秒  ·  已讀取 %llu MB",
    "已有 %u 秒沒有進展：遊戲可能已停止回應。",
    "按下 HOME 鍵並關閉軟體即可結束。",
    "鍵盤",
    "設定",
};

static const Strings STRINGS_FR = {
    "Jeu Windows",
    { "Démarrage de l'environnement Windows", "Lancement du jeu", "Première image du jeu" },
    "%u s  ·  %llu Mo lus",
    "Aucun progrès depuis %u s : le jeu ne répond peut-être plus.",
    "Appuyez sur HOME et fermez le logiciel pour quitter.",
    "clavier",
    "paramètres",
};

static const Strings STRINGS_FR_CA = {
    "Jeu Windows",
    { "Démarrage de l'environnement Windows", "Démarrage du jeu", "Première image du jeu" },
    "%u s  ·  %llu Mo lus",
    "Aucun progrès depuis %u s : le jeu ne répond peut-être plus.",
    "Appuyez sur HOME et fermez le logiciel pour quitter.",
    "clavier",
    "paramètres",
};

static const Strings STRINGS_DE = {
    "Windows-Spiel",
    { "Windows-Umgebung wird gestartet", "Spiel wird gestartet", "Das erste Bild des Spiels" },
    "%u s  ·  %llu MB gelesen",
    "Seit %u s kein Fortschritt: Das Spiel reagiert vielleicht nicht mehr.",
    "Drücke die HOME-Taste und schließe die Software, um das Spiel zu beenden.",
    "Tastatur",
    "Einstellungen",
};

static const Strings STRINGS_IT = {
    "Gioco per Windows",
    { "Avvio dell'ambiente Windows", "Avvio del gioco", "La prima immagine del gioco" },
    "%u s  ·  %llu MB letti",
    "Nessun progresso da %u s: il gioco potrebbe non rispondere più.",
    "Premi HOME e chiudi il software per uscire.",
    "tastiera",
    "impostazioni",
};

static const Strings STRINGS_ES = {
    "Juego de Windows",
    { "Iniciando el entorno de Windows", "Iniciando el juego", "Primera imagen del juego" },
    "%u s  ·  %llu MB leídos",
    "Sin avances desde hace %u s: puede que el juego no responda.",
    "Pulsa el botón HOME y cierra el programa para salir.",
    "teclado",
    "ajustes",
};

static const Strings STRINGS_ES_419 = {
    "Juego de Windows",
    { "Iniciando el entorno de Windows", "Iniciando el juego", "Primera imagen del juego" },
    "%u s  ·  %llu MB leídos",
    "Sin avances durante %u s: puede que el juego no responda.",
    "Presiona el botón HOME y cierra el programa para salir.",
    "teclado",
    "configuración",
};

static const Strings STRINGS_NL = {
    "Windows-game",
    { "Windows-omgeving starten", "Game starten", "Eerste beeld van de game" },
    "%u s  ·  %llu MB gelezen",
    "Al %u s geen voortgang: de game reageert misschien niet meer.",
    "Druk op de HOME-knop en sluit de software om te stoppen.",
    "toetsenbord",
    "instellingen",
};

static const Strings STRINGS_PT = {
    "Jogo para Windows",
    { "A iniciar o ambiente Windows", "A iniciar o jogo", "A primeira imagem do jogo" },
    "%u s  ·  %llu MB lidos",
    "Sem progresso há %u s: o jogo pode ter deixado de responder.",
    "Prima o botão HOME e feche o software para sair.",
    "teclado",
    "definições",
};

static const Strings STRINGS_PT_BR = {
    "Jogo para Windows",
    { "Iniciando o ambiente Windows", "Iniciando o jogo", "Primeira imagem do jogo" },
    "%u s  ·  %llu MB lidos",
    "Sem progresso há %u s: o jogo pode ter parado de responder.",
    "Pressione HOME e feche o software para sair.",
    "teclado",
    "configurações",
};

static const Strings* const STRINGS[CarafeLanguageCount] = {
    [CarafeLanguageRu] = &STRINGS_RU,
    [CarafeLanguageEn] = &STRINGS_EN,
    [CarafeLanguageZhHans] = &STRINGS_ZH_HANS,
    [CarafeLanguageJa] = &STRINGS_JA,
    [CarafeLanguageKo] = &STRINGS_KO,
    [CarafeLanguageZhHant] = &STRINGS_ZH_HANT,
    [CarafeLanguageFr] = &STRINGS_FR,
    [CarafeLanguageFrCa] = &STRINGS_FR_CA,
    [CarafeLanguageDe] = &STRINGS_DE,
    [CarafeLanguageIt] = &STRINGS_IT,
    [CarafeLanguageEs] = &STRINGS_ES,
    [CarafeLanguageEs419] = &STRINGS_ES_419,
    [CarafeLanguageNl] = &STRINGS_NL,
    [CarafeLanguagePt] = &STRINGS_PT,
    [CarafeLanguagePtBr] = &STRINGS_PT_BR,
};

extern unsigned int wine_nx_gl_swaps __attribute__((weak));
extern unsigned int wine_nx_vk_presents __attribute__((weak));
extern unsigned int wine_nx_dialogs_shown __attribute__((weak));
extern int wine_nx_window_picture_shown __attribute__((weak));
void wine_nx_compositor_redraw(void);
void wine_nx_threads_report_stalled(void);

int __real_wine_nx_osk_visible(void);
unsigned int __real_wine_nx_osk_generation(void);
int __real_wine_nx_osk_frame(int screen_width, int screen_height, struct wine_nx_osk_frame* frame);
unsigned int __real_wine_nx_osk_copy(int screen_width, int screen_height, void* pixels, int stride, int rgba);
void __real_wine_nx_compositor_cursor(int x, int y, int visible);

typedef void (*GetIntegervFn)(GLenum name, GLint* value);
typedef void (*BindFramebufferFn)(GLenum target, GLuint framebuffer);
typedef void (*BindBufferFn)(GLenum target, GLuint buffer);
typedef void (*PixelStoreiFn)(GLenum name, GLint value);
typedef void (*ReadPixelsFn)(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* data);

typedef struct {
    int step;
    char details[96];
    char stall[128];
} Status;

typedef struct {
    uint32_t* pixels;
    int width;
    int height;
    unsigned int generation;
    unsigned int static_generation;
} Picture;

typedef struct {
    uint32_t* pixels;
    int width;
    int height;
} Image;

typedef struct {
    uint8_t* indices;
    uint32_t palette[256];
    unsigned int delay_ms;
} MovieFrame;

typedef struct {
    MovieFrame* frames;
    int count;
    int width;
    int height;
    unsigned int total_ms;
} Movie;

typedef struct {
    const uint8_t* data;
    size_t size;
    size_t pos;
} Reader;

typedef struct {
    Reader* reader;
    int block;
    bool ended;
    uint32_t bits;
    int count;
} BitReader;

typedef struct {
    TTF_Font* font;
    int size;
} FontSlot;

bool __nx_applet_auto_notifyrunning = false;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_enabled;
static bool g_active;
static unsigned int g_pictures_copied;
static bool g_shown;
static char g_title[128];
static const Strings* g_strings = &STRINGS_EN;
static Image g_icon;
static Image g_splash;
static Movie g_movie;
static unsigned int g_generation = GENERATION_BASE;
static unsigned int g_static_generation = 1;
static unsigned int g_loader_step;
static Status g_status;
static u64 g_start_tick;
static u64 g_progress_tick;
static u64 g_game_tick;
static u64 g_last_bytes;
static unsigned int g_last_frames;
static unsigned int g_first_vulkan;
static bool g_vulkan_seen;
static u64 g_reported_tick;
static bool g_report_due;

static Picture g_osk_picture;

static pthread_mutex_t g_cursor_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_cursor_x;
static int g_cursor_y;
static int g_cursor_visible = 1;
static bool g_cursor_known;
static bool g_cursor_hidden;

static FontSlot g_fonts[FONT_SLOTS];
static int g_font_next;
static bool g_font_failed;
static bool g_menu_available;

static __thread bool t_gl_probe;
static u64 g_probe_tick;
static int g_probe_hits;
static bool g_gl_ready;
static bool g_gl_failed;
static GetIntegervFn p_glGetIntegerv;
static BindFramebufferFn p_glBindFramebuffer;
static BindBufferFn p_glBindBuffer;
static PixelStoreiFn p_glPixelStorei;
static ReadPixelsFn p_glReadPixels;

static void trace(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

static void trace(const char* fmt, ...) {
    char line[256];
    int prefix = snprintf(line, sizeof(line), "[CARAFE] ");
    va_list args;
    va_start(args, fmt);
    vsnprintf(line + prefix, sizeof(line) - prefix, fmt, args);
    va_end(args);

    carafeOverlayLog(line);
}

static unsigned int secondsBetween(u64 from, u64 to) {
    return (unsigned int)(armTicksToNs(to - from) / 1000000000ULL);
}

static unsigned int glFrames(void) {
    return &wine_nx_gl_swaps ? __atomic_load_n(&wine_nx_gl_swaps, __ATOMIC_RELAXED) : 0;
}

static unsigned int vulkanFrames(void) {
    return &wine_nx_vk_presents ? __atomic_load_n(&wine_nx_vk_presents, __ATOMIC_RELAXED) : 0;
}

static bool loadingActive(void) {
    return g_enabled && __atomic_load_n(&g_active, __ATOMIC_ACQUIRE);
}

static void finish(const char* reason) {
    if (!g_active) {
        return;
    }

    __atomic_store_n(&g_active, false, __ATOMIC_RELEASE);
    g_generation++;
    appletNotifyRunning(NULL);
    trace("loading screen hidden after %u s: %s", secondsBetween(g_start_tick, armGetSystemTick()), reason);
}

static void checkFinish(unsigned int gl, unsigned int vulkan) {
    if (__real_wine_nx_osk_visible()) {
        finish("the program opened the keyboard");
        return;
    }
    if (vulkan != 0 && !g_vulkan_seen) {
        g_vulkan_seen = true;
        g_first_vulkan = vulkan;
    }
    if (g_vulkan_seen && vulkan - g_first_vulkan >= VULKAN_FRAMES_SHOWN) {
        finish("Vulkan frames");
        return;
    }
    if (g_game_tick == 0) {
        return;
    }
    if (&wine_nx_window_picture_shown && __atomic_load_n(&wine_nx_window_picture_shown, __ATOMIC_ACQUIRE)) {
        finish("the game drew a picture in its window");
        return;
    }
    if (gl != 0 || vulkan != 0) {
        return;
    }
    if (&wine_nx_dialogs_shown && __atomic_load_n(&wine_nx_dialogs_shown, __ATOMIC_RELAXED) != 0) {
        finish("the program shows a message");
    }
}

static void updateStatus(void) {
    u64 now = armGetSystemTick();
    u64 bytes = carafeOverlayBytesRead();
    unsigned int gl = glFrames();
    unsigned int vulkan = vulkanFrames();
    unsigned int frames = gl + vulkan;

    if (bytes != g_last_bytes || frames != g_last_frames) {
        g_last_bytes = bytes;
        g_last_frames = frames;
        g_progress_tick = now;
    }
    if (g_game_tick == 0 && carafeOverlayGameOpened()) {
        g_game_tick = now;
    }

    unsigned int elapsed = secondsBetween(g_start_tick, now);
    unsigned int idle = secondsBetween(g_progress_tick, now);
    Status status;
    memset(&status, 0, sizeof(status));
    status.step = frames != 0 ? 2 : g_game_tick != 0 ? 1 : 0;
    snprintf(status.details, sizeof(status.details), g_strings->details, elapsed,
        (unsigned long long)(bytes >> 20));
    if (idle >= STALL_SECONDS) {
        snprintf(status.stall, sizeof(status.stall), g_strings->stall, idle);
        if (g_reported_tick != g_progress_tick) {
            g_reported_tick = g_progress_tick;
            g_report_due = true;
        }
    }

    if (memcmp(&status, &g_status, sizeof(status)) != 0) {
        g_status = status;
        g_static_generation++;
        g_generation++;
    }
    unsigned int step = (unsigned int)(armTicksToNs(now - g_start_tick) / 1000000ULL / LOADER_STEP_MS);
    if (step != g_loader_step) {
        g_loader_step = step;
        g_generation++;
    }
    checkFinish(gl, vulkan);
}

static TTF_Font* fontAt(int size) {
    for (int i = 0; i < FONT_SLOTS; i++) {
        if (g_fonts[i].font != NULL && g_fonts[i].size == size) {
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
    if (!wine_nx_osk_font(&data, &bytes)) {
        g_font_failed = true;
        return NULL;
    }

    FontSlot* slot = &g_fonts[g_font_next++ % FONT_SLOTS];
    if (slot->font != NULL) {
        TTF_CloseFont(slot->font);
    }
    slot->font = TTF_OpenFontRW(SDL_RWFromConstMem(data, (int)bytes), 1, size);
    slot->size = size;
    return slot->font;
}

static uint32_t blend(uint32_t under, uint32_t color, unsigned int alpha) {
    uint32_t r = ((under >> 16 & 0xFF) * (255 - alpha) + (color >> 16 & 0xFF) * alpha) / 255;
    uint32_t g = ((under >> 8 & 0xFF) * (255 - alpha) + (color >> 8 & 0xFF) * alpha) / 255;
    uint32_t b = ((under & 0xFF) * (255 - alpha) + (color & 0xFF) * alpha) / 255;
    return 0xFF000000u | r << 16 | g << 8 | b;
}

static void fillRect(Picture* picture, int x0, int y0, int w, int h, uint32_t color) {
    for (int y = y0 < 0 ? 0 : y0; y < y0 + h && y < picture->height; y++) {
        for (int x = x0 < 0 ? 0 : x0; x < x0 + w && x < picture->width; x++) {
            picture->pixels[y * picture->width + x] = color;
        }
    }
}

static int scaled(int value, int width) {
    return (int)(((int64_t)value * width + 640) / 1280);
}

static int atLeastOne(int value) {
    return value < 1 ? 1 : value;
}

static int drawText(Picture* picture, const char* text, int size, int x0, int top, int max_width, uint32_t color) {
    if (text[0] == '\0' || size < 8 || max_width <= 0) {
        return 0;
    }

    TTF_Font* font = fontAt(size);
    if (font == NULL) {
        return 0;
    }
    int w = carafeTextMeasure(font, size, false, text);
    if (w <= 0) {
        return 0;
    }
    if (w > max_width) {
        size = size * max_width / w;
        if (size < 8) {
            return 0;
        }
        font = fontAt(size);
        if (font == NULL) {
            return 0;
        }
    }

    SDL_Surface* surface = carafeTextRender(font, size, false, text);
    if (surface == NULL) {
        return 0;
    }

    for (int y = 0; y < surface->h; y++) {
        const uint32_t* row = (const uint32_t*)((const uint8_t*)surface->pixels + y * surface->pitch);
        for (int x = 0; x < surface->w; x++) {
            int px = x0 + x;
            int py = top + y;
            unsigned int alpha = row[x] >> 24;
            if (alpha != 0 && px >= 0 && py >= 0 && px < picture->width && py < picture->height) {
                uint32_t* pixel = &picture->pixels[py * picture->width + px];
                *pixel = blend(*pixel, color, alpha);
            }
        }
    }
    int height = surface->h;
    SDL_FreeSurface(surface);
    return height;
}

static int splashSize(int file_size, int width) {
    return (int)((int64_t)file_size * SPLASH_SCALE_NUM * width / (SPLASH_SCALE_DEN * 1280));
}

static void drawSplash(Picture* picture) {
    if (g_splash.pixels == NULL) {
        return;
    }

    int w = picture->width;
    int left = scaled(SPLASH_LOGO_LEFT, w);
    int top = scaled(SPLASH_LOGO_TOP, w);
    int width = splashSize(g_splash.width, w);
    int height = splashSize(g_splash.height, w) * SPLASH_LOGO_HEIGHT_NUM / SPLASH_LOGO_HEIGHT_DEN;
    for (int y = 0; y < height; y++) {
        int py = top + y;
        if (py < 0 || py >= picture->height) {
            continue;
        }
        const uint32_t* row = g_splash.pixels + (size_t)(y * g_splash.height / height) * g_splash.width;
        for (int x = 0; x < width; x++) {
            int px = left + x;
            if (px >= 0 && px < picture->width) {
                picture->pixels[py * picture->width + px] = 0xFF000000u | row[x * g_splash.width / width];
            }
        }
    }
}

static const MovieFrame* currentMovieFrame(void) {
    if (g_movie.count == 0 || g_movie.total_ms == 0) {
        return NULL;
    }

    unsigned int at = g_loader_step * LOADER_STEP_MS % g_movie.total_ms;
    for (int i = 0; i < g_movie.count; i++) {
        if (at < g_movie.frames[i].delay_ms) {
            return &g_movie.frames[i];
        }
        at -= g_movie.frames[i].delay_ms;
    }
    return &g_movie.frames[g_movie.count - 1];
}

static void drawMovie(Picture* picture) {
    const MovieFrame* frame = currentMovieFrame();
    if (frame == NULL) {
        return;
    }

    int w = picture->width;
    int width = splashSize(g_movie.width, w);
    int height = splashSize(g_movie.height, w);
    int left = scaled(SPLASH_MOVIE_RIGHT, w) - width;
    int top = scaled(SPLASH_MOVIE_BOTTOM, w) - height;
    for (int y = 0; y < height; y++) {
        int py = top + y;
        if (py < 0 || py >= picture->height) {
            continue;
        }
        const uint8_t* row = frame->indices + (size_t)(y * g_movie.height / height) * g_movie.width;
        for (int x = 0; x < width; x++) {
            int px = left + x;
            if (px >= 0 && px < picture->width) {
                uint32_t color = frame->palette[row[x * g_movie.width / width]];
                picture->pixels[py * picture->width + px] = 0xFF000000u | color;
            }
        }
    }
}

static bool drawIcon(Picture* picture, int left, int top, int size, int radius) {
    if (g_icon.pixels == NULL || size <= 0) {
        return false;
    }

    for (int y = 0; y < size; y++) {
        int py = top + y;
        if (py < 0 || py >= picture->height) {
            continue;
        }
        int ey = y < radius ? radius - y : y >= size - radius ? y - (size - radius - 1) : 0;
        const uint32_t* row = g_icon.pixels + (size_t)(y * g_icon.height / size) * g_icon.width;
        for (int x = 0; x < size; x++) {
            int px = left + x;
            int ex = x < radius ? radius - x : x >= size - radius ? x - (size - radius - 1) : 0;
            if (px < 0 || px >= picture->width || ex * ex + ey * ey > radius * radius) {
                continue;
            }
            picture->pixels[py * picture->width + px] = 0xFF000000u | row[x * g_icon.width / size];
        }
    }
    return true;
}

static void drawMarker(Picture* picture, int cx, int cy, int radius, int thickness, int state) {
    int inner = radius - thickness;
    int dot = radius * 2 / 5;
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 > radius * radius) {
                continue;
            }
            uint32_t color = 0;
            if (state < 0) {
                color = COLOR_STEP_DONE;
            } else if (d2 > inner * inner) {
                color = state == 0 ? COLOR_STEP_NOW : COLOR_STEP_LATER;
            } else if (state == 0 && d2 <= dot * dot) {
                color = COLOR_STEP_NOW;
            }
            if (color != 0) {
                fillRect(picture, cx + dx, cy + dy, 1, 1, color);
            }
        }
    }
}

static void drawPill(Picture* picture, int x, int y, int w, int h, uint32_t fill, uint32_t edge) {
    int radius = h / 2;
    for (int py = 0; py < h; py++) {
        for (int px = 0; px < w; px++) {
            int cx = px < radius ? radius : px >= w - radius ? w - radius - 1 : px;
            int dx = px - cx;
            int dy = py - radius;
            int d2 = dx * dx + dy * dy;
            if (d2 > radius * radius) {
                continue;
            }
            uint32_t color = d2 > (radius - 2) * (radius - 2) ? edge : fill;
            fillRect(picture, x + px, y + py, 1, 1, color);
        }
    }
}

static int drawChord(Picture* picture, const char* const* keys, int count, int x, int y, int size) {
    int w = picture->width;
    int height = scaled(30, w);
    for (int i = 0; i < count; i++) {
        TTF_Font* font = fontAt(size);
        int tw = 0;
        int th = 0;
        if (font != NULL) {
            TTF_SizeUTF8(font, keys[i], &tw, &th);
        }
        int pill = tw + scaled(18, w) < height ? height : tw + scaled(18, w);
        drawPill(picture, x, y, pill, height, 0xFF231B20u, 0xFF4A3D49u);
        drawText(picture, keys[i], size, x + (pill - tw) / 2, y + (height - th) / 2, pill, COLOR_TITLE);
        x += pill;
        if (i + 1 < count) {
            int pw = 0;
            int ph = 0;
            if (font != NULL) {
                TTF_SizeUTF8(font, "+", &pw, &ph);
            }
            drawText(picture, "+", size, x + scaled(5, w), y + (height - ph) / 2, pw + 2, COLOR_STEP_LATER);
            x += pw + scaled(10, w);
        }
    }
    return x;
}

static void drawHints(Picture* picture) {
    static const char* const KEYBOARD[] = { "−", "B" };
    static const char* const SETTINGS[] = { "−", "+", "L", "R" };
    int w = picture->width;
    int x = scaled(LAYOUT_MARGIN, w);
    int size = w * 15 / 1280;
    int line = scaled(40, w);
    int y = scaled(600, w);
    int end = drawChord(picture, KEYBOARD, 2, x, y, size);
    drawText(picture, g_strings->keyboard, w * 16 / 1280, end + scaled(12, w), y + scaled(5, w), w, COLOR_TEXT);
    if (g_menu_available) {
        end = drawChord(picture, SETTINGS, 4, x, y + line, size);
        drawText(picture, g_strings->settings, w * 16 / 1280, end + scaled(12, w), y + line + scaled(5, w), w,
            COLOR_TEXT);
    }
}

static void drawGame(Picture* picture) {
    int w = picture->width;
    int left = scaled(LAYOUT_MARGIN, w);
    int top = scaled(LAYOUT_LOGO_TOP + LAYOUT_LOGO_HEIGHT + LAYOUT_MARGIN, w);
    int text_x = left;
    if (drawIcon(picture, left, top, w * 19 / 100, w * 12 / 1000)) {
        text_x = left + w * 19 / 100 + w * 34 / 1000;
    }

    int max_width = w - text_x - left;
    int title_size = w * 32 / 1280;
    int y = top + w * 19 / 100 / 8;
    int line = drawText(picture, g_title, title_size, text_x, y, max_width, COLOR_TITLE);
    y += (line != 0 ? line : title_size) + w * 22 / 1280;

    int step_size = w * 21 / 1280;
    int row = w * 38 / 1280;
    int radius = atLeastOne(w * 7 / 1280);
    int thickness = atLeastOne(w * 2 / 1280);
    for (int i = 0; i < STEP_COUNT; i++) {
        int state = i < g_status.step ? -1 : i == g_status.step ? 0 : 1;
        drawMarker(picture, text_x + radius, y + step_size * 3 / 5, radius, thickness, state);
        uint32_t color = state == 0 ? COLOR_TITLE : state < 0 ? COLOR_TEXT : COLOR_DIM;
        drawText(picture, g_strings->steps[i], step_size, text_x + radius * 2 + w * 14 / 1280, y,
            max_width - radius * 2 - w * 14 / 1280, color);
        y += row;
    }

    y += w * 14 / 1280;
    line = drawText(picture, g_status.details, w * 18 / 1280, text_x, y, max_width, COLOR_DIM);
    y += (line != 0 ? line : w * 24 / 1280) + w * 14 / 1280;

    if (g_status.stall[0] != '\0') {
        line = drawText(picture, g_status.stall, w * 18 / 1280, text_x, y, max_width, COLOR_WARN);
        y += (line != 0 ? line : w * 24 / 1280) + w * 4 / 1280;
        drawText(picture, g_strings->hint, w * 16 / 1280, text_x, y, max_width, COLOR_TEXT);
    }

    drawHints(picture);
}

static bool renderPicture(Picture* picture, int width, int height) {
    if (picture->pixels == NULL || picture->width != width || picture->height != height) {
        free(picture->pixels);
        picture->pixels = malloc((size_t)width * height * 4);
        if (picture->pixels == NULL) {
            picture->width = 0;
            picture->height = 0;
            return false;
        }
        picture->width = width;
        picture->height = height;
    }

    fillRect(picture, 0, 0, width, height, COLOR_BACKGROUND);
    drawSplash(picture);
    drawGame(picture);
    drawMovie(picture);
    picture->static_generation = g_static_generation;
    picture->generation = g_generation;
    if (!g_shown) {
        g_shown = true;
        trace("loading screen shown at %dx%d", width, height);
    }
    return true;
}

static bool pictureStale(const Picture* picture, int width, int height) {
    return picture->pixels == NULL || picture->width != width || picture->height != height
        || picture->static_generation != g_static_generation || picture->generation != g_generation;
}

static void copyPicture(const Picture* picture, void* pixels, int stride, bool rgba) {
    for (int y = 0; y < picture->height; y++) {
        const uint32_t* src = picture->pixels + (size_t)y * picture->width;
        uint32_t* dst = (uint32_t*)((uint8_t*)pixels + (size_t)y * stride);
        if (!rgba) {
            memcpy(dst, src, (size_t)picture->width * 4);
            continue;
        }
        for (int x = 0; x < picture->width; x++) {
            uint32_t bgra = src[x];
            dst[x] = (bgra & 0xFF00FF00u) | (bgra & 0xFFu) << 16 | (bgra >> 16 & 0xFFu);
        }
    }
}

static bool loadGl(void) {
    if (g_gl_ready) {
        return true;
    }
    if (g_gl_failed) {
        return false;
    }

    p_glGetIntegerv = (GetIntegervFn)eglGetProcAddress("glGetIntegerv");
    p_glBindFramebuffer = (BindFramebufferFn)eglGetProcAddress("glBindFramebuffer");
    p_glBindBuffer = (BindBufferFn)eglGetProcAddress("glBindBuffer");
    p_glPixelStorei = (PixelStoreiFn)eglGetProcAddress("glPixelStorei");
    p_glReadPixels = (ReadPixelsFn)eglGetProcAddress("glReadPixels");
    g_gl_ready = p_glGetIntegerv != NULL && p_glBindFramebuffer != NULL && p_glBindBuffer != NULL
        && p_glPixelStorei != NULL && p_glReadPixels != NULL;
    g_gl_failed = !g_gl_ready;
    if (g_gl_failed) {
        trace("frame check unavailable: OpenGL functions not found");
    }
    return g_gl_ready;
}

static int countContent(const uint8_t* rgba, int count) {
    int content = 0;
    for (int i = 0; i < count; i++) {
        const uint8_t* p = rgba + i * 4;
        if (p[0] + p[1] + p[2] > PROBE_BRIGHTNESS) {
            content++;
        }
    }
    return content;
}

static int sampleBackBuffer(int w, int h) {
    int longest = w > h ? w : h;
    uint8_t* line = malloc((size_t)longest * 4);
    if (line == NULL) {
        return 0;
    }

    GLint read_fb = 0;
    GLint pack_buffer = 0;
    GLint alignment = 4;
    GLint row_length = 0;
    GLint skip_pixels = 0;
    GLint skip_rows = 0;
    p_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    p_glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
    p_glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
    p_glGetIntegerv(GL_PACK_ROW_LENGTH, &row_length);
    p_glGetIntegerv(GL_PACK_SKIP_PIXELS, &skip_pixels);
    p_glGetIntegerv(GL_PACK_SKIP_ROWS, &skip_rows);

    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    p_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    p_glPixelStorei(GL_PACK_ALIGNMENT, 4);
    p_glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    p_glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    p_glPixelStorei(GL_PACK_SKIP_ROWS, 0);

    int content = 0;
    const int rows[] = { h / 10, h - 1 - h / 10 };
    const int columns[] = { w / 10, w - 1 - w / 10 };
    for (int i = 0; i < 2; i++) {
        p_glReadPixels(0, rows[i], w, 1, GL_RGBA, GL_UNSIGNED_BYTE, line);
        content += countContent(line, w);
        p_glReadPixels(columns[i], 0, 1, h, GL_RGBA, GL_UNSIGNED_BYTE, line);
        content += countContent(line, h);
    }

    p_glPixelStorei(GL_PACK_SKIP_ROWS, skip_rows);
    p_glPixelStorei(GL_PACK_SKIP_PIXELS, skip_pixels);
    p_glPixelStorei(GL_PACK_ROW_LENGTH, row_length);
    p_glPixelStorei(GL_PACK_ALIGNMENT, alignment);
    p_glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack_buffer);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
    free(line);
    return content;
}

static void probeGlFrame(int w, int h) {
    u64 now = armGetSystemTick();
    if (armTicksToNs(now - __atomic_load_n(&g_probe_tick, __ATOMIC_RELAXED)) < PROBE_INTERVAL_NS) {
        return;
    }
    __atomic_store_n(&g_probe_tick, now, __ATOMIC_RELAXED);
    if (w <= 0 || h <= 0 || eglGetCurrentContext() == EGL_NO_CONTEXT || !loadGl()) {
        return;
    }

    int content = sampleBackBuffer(w, h);
    pthread_mutex_lock(&g_lock);
    if (content >= PROBE_CONTENT_PIXELS) {
        if (++g_probe_hits >= PROBE_CONFIRMATIONS) {
            finish("the game drew its picture");
        }
    } else {
        g_probe_hits = 0;
    }
    pthread_mutex_unlock(&g_lock);
}

int __wrap_wine_nx_osk_visible(void) {
    if (carafeMenuVisible()) {
        return 1;
    }
    if (!loadingActive()) {
        return __real_wine_nx_osk_visible();
    }
    t_gl_probe = true;
    return 1;
}

unsigned int __wrap_wine_nx_osk_generation(void) {
    unsigned int real = __real_wine_nx_osk_generation() + carafeMenuGeneration();
    if (!g_enabled) {
        return real;
    }

    pthread_mutex_lock(&g_lock);
    if (g_active) {
        updateStatus();
    }
    unsigned int ours = g_generation;
    pthread_mutex_unlock(&g_lock);
    return real + ours;
}

int __wrap_wine_nx_osk_frame(int screen_width, int screen_height, struct wine_nx_osk_frame* frame) {
    bool probe = t_gl_probe;
    t_gl_probe = false;
    if (carafeMenuVisible()) {
        if (loadingActive() && probe) {
            probeGlFrame(screen_width, screen_height);
        }
        frame->x = 0;
        frame->y = 0;
        frame->width = screen_width;
        frame->height = screen_height;
        frame->generation = carafeMenuGeneration() | 0x80000000u;
        return 1;
    }
    if (!loadingActive()) {
        return __real_wine_nx_osk_frame(screen_width, screen_height, frame);
    }
    if (probe) {
        probeGlFrame(screen_width, screen_height);
    }

    pthread_mutex_lock(&g_lock);
    if (g_active) {
        updateStatus();
    }
    bool active = g_active;
    unsigned int generation = g_generation;
    pthread_mutex_unlock(&g_lock);
    if (!active) {
        return __real_wine_nx_osk_frame(screen_width, screen_height, frame);
    }

    frame->x = 0;
    frame->y = 0;
    frame->width = screen_width;
    frame->height = screen_height;
    frame->generation = generation;
    return 1;
}

unsigned int __wrap_wine_nx_osk_copy(int screen_width, int screen_height, void* pixels, int stride, int rgba) {
    if (carafeMenuVisible()) {
        unsigned int copied = carafeMenuCopy(screen_width, screen_height, pixels, stride, rgba);
        if (copied != 0) {
            return copied;
        }
    }
    if (!loadingActive()) {
        return __real_wine_nx_osk_copy(screen_width, screen_height, pixels, stride, rgba);
    }

    pthread_mutex_lock(&g_lock);
    if (!g_active) {
        pthread_mutex_unlock(&g_lock);
        return __real_wine_nx_osk_copy(screen_width, screen_height, pixels, stride, rgba);
    }
    if (pictureStale(&g_osk_picture, screen_width, screen_height)
        && !renderPicture(&g_osk_picture, screen_width, screen_height)) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    copyPicture(&g_osk_picture, pixels, stride, rgba != 0);
    unsigned int copied = g_osk_picture.generation;
    const bool first_frame_queued = ++g_pictures_copied == 2;
    pthread_mutex_unlock(&g_lock);
    if (first_frame_queued) {
        appletNotifyRunning(NULL);
    }
    return copied;
}

int wine_nx_watch_window_pictures(void) {
    return loadingActive();
}

void __wrap_wine_nx_compositor_cursor(int x, int y, int visible) {
    pthread_mutex_lock(&g_cursor_lock);
    g_cursor_x = x;
    g_cursor_y = y;
    g_cursor_visible = visible;
    g_cursor_known = true;
    g_cursor_hidden = loadingActive() || carafeMenuVisible();
    __real_wine_nx_compositor_cursor(x, y, g_cursor_hidden ? 0 : visible);
    pthread_mutex_unlock(&g_cursor_lock);
}

static void restoreCursor(void) {
    pthread_mutex_lock(&g_cursor_lock);
    if (g_cursor_hidden && g_cursor_known) {
        g_cursor_hidden = false;
        __real_wine_nx_compositor_cursor(g_cursor_x, g_cursor_y, g_cursor_visible);
    }
    pthread_mutex_unlock(&g_cursor_lock);
}

static void reportStall(void) {
    pthread_mutex_lock(&g_lock);
    bool due = g_report_due;
    g_report_due = false;
    pthread_mutex_unlock(&g_lock);
    if (due) {
        trace("no progress for %u s, reporting the threads", STALL_SECONDS);
        wine_nx_threads_report_stalled();
    }
}

void carafeLoadingTick(void) {
    carafeSettingsTick();
    if (loadingActive()) {
        wine_nx_compositor_redraw();
        reportStall();
    } else if (g_enabled && !carafeMenuVisible()) {
        restoreCursor();
    }
}

static void chooseLanguage(void) {
    g_strings = STRINGS[carafeLanguage()];
}

static void loadIcon(void) {
    free(g_icon.pixels);
    g_icon.pixels = NULL;
    g_icon.width = 0;
    g_icon.height = 0;

    size_t size = 0;
    unsigned char* data = carafeOverlayReadRomfsFile("/carafe/icon.jpg", ICON_LIMIT, &size);
    if (data == NULL) {
        trace("no game icon available");
        return;
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
                g_icon.pixels = pixels;
                g_icon.width = (int)png.width;
                g_icon.height = (int)png.height;
            } else {
                free(pixels);
            }
        }
        png_image_free(&png);
        free(data);
        if (g_icon.pixels != NULL) {
            trace("game icon %dx%d", g_icon.width, g_icon.height);
        } else {
            trace("game icon PNG could not be decoded");
        }
        return;
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
            g_icon.pixels = pixels;
            g_icon.width = width;
            g_icon.height = height;
        } else {
            free(pixels);
        }
    }
    if (decoder != NULL) {
        tjDestroy(decoder);
    }
    free(data);
    if (g_icon.pixels != NULL) {
        trace("game icon %dx%d", g_icon.width, g_icon.height);
    } else {
        trace("game icon could not be decoded");
    }
}

static int readByte(Reader* reader) {
    return reader->pos < reader->size ? reader->data[reader->pos++] : -1;
}

static int readWord(Reader* reader) {
    int low = readByte(reader);
    int high = readByte(reader);
    return low < 0 || high < 0 ? -1 : low | high << 8;
}

static bool readPalette(Reader* reader, uint32_t* palette, int count) {
    for (int i = 0; i < count; i++) {
        int r = readByte(reader);
        int g = readByte(reader);
        int b = readByte(reader);
        if (r < 0 || g < 0 || b < 0) {
            return false;
        }
        palette[i] = (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
    }
    return true;
}

static bool skipBlocks(Reader* reader) {
    for (;;) {
        int length = readByte(reader);
        if (length < 0) {
            return false;
        }
        if (length == 0) {
            return true;
        }
        reader->pos += (size_t)length;
        if (reader->pos > reader->size) {
            return false;
        }
    }
}

static int readCode(BitReader* in, int size) {
    while (in->count < size) {
        if (in->block == 0) {
            int length = in->ended ? -1 : readByte(in->reader);
            if (length <= 0) {
                in->ended = true;
                return -1;
            }
            in->block = length;
        }
        int value = readByte(in->reader);
        if (value < 0) {
            return -1;
        }
        in->block--;
        in->bits |= (uint32_t)value << in->count;
        in->count += 8;
    }
    int code = (int)(in->bits & ((1u << size) - 1));
    in->bits >>= size;
    in->count -= size;
    return code;
}

static bool decodeLzw(Reader* reader, uint8_t* out, size_t total) {
    static uint16_t prefix[LZW_CODES];
    static uint8_t suffix[LZW_CODES];
    static uint8_t stack[LZW_CODES + 1];

    int minimum = readByte(reader);
    if (minimum < 2 || minimum > 8) {
        return false;
    }
    int clear = 1 << minimum;
    int end = clear + 1;
    int size = minimum + 1;
    int next = end + 1;
    int previous = -1;
    uint8_t first = 0;
    size_t written = 0;
    BitReader in = { reader, 0, false, 0, 0 };
    for (int i = 0; i < clear; i++) {
        suffix[i] = (uint8_t)i;
    }

    for (;;) {
        int code = readCode(&in, size);
        if (code < 0 || code == end) {
            break;
        }
        if (code == clear) {
            size = minimum + 1;
            next = end + 1;
            previous = -1;
            continue;
        }
        if (previous < 0) {
            if (code >= clear) {
                return false;
            }
            first = (uint8_t)code;
            if (written < total) {
                out[written++] = first;
            }
            previous = code;
            continue;
        }

        int current = code;
        int top = 0;
        if (code >= next) {
            if (code > next) {
                return false;
            }
            stack[top++] = first;
            current = previous;
        }
        while (current > end) {
            if (top >= LZW_CODES) {
                return false;
            }
            stack[top++] = suffix[current];
            current = prefix[current];
        }
        if (current >= clear) {
            return false;
        }
        first = (uint8_t)current;
        stack[top++] = first;
        if (next < LZW_CODES) {
            prefix[next] = (uint16_t)previous;
            suffix[next] = first;
            next++;
            if (next == 1 << size && size < 12) {
                size++;
            }
        }
        while (top > 0 && written < total) {
            out[written++] = stack[--top];
        }
        previous = code;
    }

    if (!in.ended) {
        reader->pos += (size_t)in.block;
        if (reader->pos > reader->size || !skipBlocks(reader)) {
            return false;
        }
    }
    return written == total;
}

static void freeMovieFrames(MovieFrame* frames, int count) {
    for (int i = 0; i < count; i++) {
        free(frames[i].indices);
    }
    free(frames);
}

static bool decodeMovie(const uint8_t* data, size_t size, Movie* movie) {
    if (size < 13 || memcmp(data, "GIF8", 4) != 0) {
        return false;
    }

    Reader reader = { data, size, 6 };
    int width = readWord(&reader);
    int height = readWord(&reader);
    int flags = readByte(&reader);
    int background = readByte(&reader);
    readByte(&reader);
    if (width <= 0 || height <= 0 || width > SPLASH_MAX_SIDE || height > SPLASH_MAX_SIDE || background < 0) {
        return false;
    }
    uint32_t global[256] = { 0 };
    if ((flags & 0x80) != 0 && !readPalette(&reader, global, 2 << (flags & 7))) {
        return false;
    }

    size_t pixels = (size_t)width * height;
    uint8_t* canvas = malloc(pixels);
    uint8_t* patch = malloc(pixels);
    MovieFrame* frames = calloc(MOVIE_MAX_FRAMES, sizeof(MovieFrame));
    int count = 0;
    bool complete = false;
    unsigned int delay_ms = 0;
    int transparent = -1;
    if (canvas != NULL && patch != NULL && frames != NULL) {
        memset(canvas, background, pixels);
        for (;;) {
            int kind = readByte(&reader);
            if (kind == 0x3B) {
                complete = count > 0;
                break;
            }
            if (kind == 0x21) {
                int label = readByte(&reader);
                if (label == 0xF9) {
                    int length = readByte(&reader);
                    int packed = readByte(&reader);
                    int delay = readWord(&reader);
                    int index = readByte(&reader);
                    if (length != 4 || packed < 0 || delay < 0 || index < 0) {
                        break;
                    }
                    delay_ms = (unsigned int)delay * 10;
                    transparent = (packed & 1) != 0 ? index : -1;
                }
                if (label < 0 || !skipBlocks(&reader)) {
                    break;
                }
                continue;
            }
            if (kind != 0x2C || count >= MOVIE_MAX_FRAMES) {
                break;
            }

            int x0 = readWord(&reader);
            int y0 = readWord(&reader);
            int w0 = readWord(&reader);
            int h0 = readWord(&reader);
            int packed = readByte(&reader);
            if (packed < 0 || x0 < 0 || y0 < 0 || w0 <= 0 || h0 <= 0 || x0 + w0 > width || y0 + h0 > height
                || (packed & 0x40) != 0) {
                break;
            }
            MovieFrame* frame = &frames[count];
            memcpy(frame->palette, global, sizeof(global));
            if ((packed & 0x80) != 0 && !readPalette(&reader, frame->palette, 2 << (packed & 7))) {
                break;
            }
            if (!decodeLzw(&reader, patch, (size_t)w0 * h0)) {
                break;
            }
            for (int y = 0; y < h0; y++) {
                for (int x = 0; x < w0; x++) {
                    uint8_t index = patch[y * w0 + x];
                    if (index != transparent) {
                        canvas[(size_t)(y0 + y) * width + x0 + x] = index;
                    }
                }
            }
            frame->indices = malloc(pixels);
            if (frame->indices == NULL) {
                break;
            }
            memcpy(frame->indices, canvas, pixels);
            frame->delay_ms = delay_ms != 0 ? delay_ms : MOVIE_DEFAULT_DELAY_MS;
            count++;
            delay_ms = 0;
            transparent = -1;
        }
    }
    free(canvas);
    free(patch);
    if (!complete) {
        if (frames != NULL) {
            freeMovieFrames(frames, count);
        }
        return false;
    }

    movie->frames = frames;
    movie->count = count;
    movie->width = width;
    movie->height = height;
    movie->total_ms = 0;
    for (int i = 0; i < count; i++) {
        movie->total_ms += frames[i].delay_ms;
    }
    return true;
}

static void loadMovie(const char* path) {
    if (g_movie.frames != NULL) {
        freeMovieFrames(g_movie.frames, g_movie.count);
        memset(&g_movie, 0, sizeof(g_movie));
    }
    size_t size = 0;
    unsigned char* data = carafeOverlayReadRomfsFile(path, SPLASH_LIMIT, &size);
    if (data == NULL) {
        return;
    }

    bool decoded = decodeMovie(data, size, &g_movie);
    free(data);
    if (decoded) {
        trace("%s %dx%d, %d frames, %u ms", path, g_movie.width, g_movie.height, g_movie.count, g_movie.total_ms);
    } else {
        trace("%s could not be decoded", path);
    }
}

static void loadSplash(const char* path, Image* image) {
    free(image->pixels);
    image->pixels = NULL;
    image->width = 0;
    image->height = 0;

    size_t size = 0;
    unsigned char* data = carafeOverlayReadRomfsFile(path, SPLASH_LIMIT, &size);
    if (data == NULL) {
        return;
    }

    png_image png;
    memset(&png, 0, sizeof(png));
    png.version = PNG_IMAGE_VERSION;
    uint32_t* pixels = NULL;
    if (png_image_begin_read_from_memory(&png, data, size) && png.width > 0 && png.height > 0
        && png.width <= SPLASH_MAX_SIDE && png.height <= SPLASH_MAX_SIDE) {
        png.format = PNG_FORMAT_BGRA;
        pixels = malloc(PNG_IMAGE_SIZE(png));
        if (pixels != NULL && !png_image_finish_read(&png, NULL, pixels, 0, NULL)) {
            free(pixels);
            pixels = NULL;
        }
    }
    png_image_free(&png);
    free(data);
    if (pixels == NULL) {
        trace("%s could not be decoded", path);
        return;
    }

    image->pixels = pixels;
    image->width = (int)png.width;
    image->height = (int)png.height;
    trace("%s %dx%d", path, image->width, image->height);
}

void carafeLoadingStart(void) {
    char target[512];
    if (carafeOverlayProgramTarget == NULL || !carafeOverlayProgramTarget(target, sizeof(target))) {
        return;
    }

    chooseLanguage();
    size_t menu_size = 0;
    void* menu_data = carafeOverlayReadRomfsFile("/carafe/menu.txt", 1024 * 1024, &menu_size);
    g_menu_available = menu_data != NULL && menu_size > 0;
    free(menu_data);

    snprintf(g_title, sizeof(g_title), "%s", g_strings->title);
    loadIcon();
    loadSplash("/carafe/loading/NintendoLogo.png", &g_splash);
    loadMovie("/carafe/loading/StartupMovie.gif");

    char title[sizeof(g_title)];
    if (carafeOverlayReadRomfsText("/carafe/title", title, sizeof(title))) {
        title[strcspn(title, "\r\n")] = '\0';
        if (title[0] != '\0') {
            snprintf(g_title, sizeof(g_title), "%s", title);
        }
    }

    pthread_mutex_lock(&g_lock);
    g_start_tick = armGetSystemTick();
    g_progress_tick = g_start_tick;
    g_game_tick = 0;
    g_last_bytes = 0;
    g_last_frames = 0;
    g_first_vulkan = 0;
    g_vulkan_seen = false;
    g_reported_tick = 0;
    g_report_due = false;
    g_pictures_copied = 0;
    g_shown = false;
    g_probe_hits = 0;
    g_loader_step = 0;
    memset(&g_status, 0, sizeof(g_status));
    g_static_generation++;
    g_generation++;
    if (carafeErrorResetStartTick != NULL) {
        carafeErrorResetStartTick();
    }
    g_active = true;
    g_enabled = true;
    pthread_mutex_unlock(&g_lock);
}
