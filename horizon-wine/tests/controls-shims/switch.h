#ifndef CONTROLS_TEST_SWITCH_H
#define CONTROLS_TEST_SWITCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uint32_t Result;

#define R_SUCCEEDED(x) ((x) == 0)
#define R_FAILED(x) ((x) != 0)
#define CUR_PROCESS_HANDLE 0xffff8001u
#define InfoType_ProgramId 18

static inline Result svcGetInfo( u64 *out, u32 id, u32 handle, u64 sub )
{
    (void)id; (void)handle; (void)sub;
    *out = 0x0100000000001000ULL;
    return 0;
}

typedef struct { int unused; } FsStorage;
static inline Result fsOpenDataStorageByCurrentProcess( FsStorage *out ) { (void)out; return 1; }
static inline void fsStorageClose( FsStorage *s ) { (void)s; }
static inline Result romfsMountFromStorage( FsStorage s, u64 offset, const char *name )
{
    (void)s; (void)offset; (void)name;
    return 1;
}

extern u64 controls_test_tick __attribute__((weak));
static inline u64 armGetSystemTick( void ) { return &controls_test_tick != NULL ? controls_test_tick : 19200000ull; }
static inline u64 armTicksToNs( u64 tick ) { return (tick * 1000ull) / 192ull * 10ull; }

typedef enum {
    PlSharedFontType_Standard,
    PlSharedFontType_ChineseSimplified,
    PlSharedFontType_ExtChineseSimplified,
    PlSharedFontType_ChineseTraditional,
    PlSharedFontType_KO,
} PlSharedFontType;
typedef enum { PlServiceType_User } PlServiceType;
typedef struct { void *address; size_t size; } PlFontData;
static inline Result plInitialize( PlServiceType t ) { (void)t; return 1; }
static inline Result plGetSharedFontByType( PlFontData *d, PlSharedFontType t ) { (void)d; (void)t; return 1; }

typedef enum {
    SetLanguage_ENUS = 1, SetLanguage_FR = 2, SetLanguage_DE = 3, SetLanguage_IT = 4,
    SetLanguage_ES = 5, SetLanguage_ZHCN = 6, SetLanguage_KO = 7, SetLanguage_NL = 8,
    SetLanguage_PT = 9, SetLanguage_RU = 10, SetLanguage_JA = 11, SetLanguage_ZHTW = 12,
    SetLanguage_FRCA = 13, SetLanguage_ES419 = 14, SetLanguage_ZHHANS = 15,
    SetLanguage_ZHHANT = 16, SetLanguage_PTBR = 17,
} SetLanguage;
static inline Result setInitialize( void ) { return 0; }
static inline void setExit( void ) {}
static inline Result setGetSystemLanguage( u64 *c ) { *c = 0; return 0; }
static inline Result setMakeLanguage( u64 c, SetLanguage *l ) { (void)c; *l = SetLanguage_ENUS; return 0; }

#define HidNpadButton_A      (1ULL << 0)
#define HidNpadButton_B      (1ULL << 1)
#define HidNpadButton_X      (1ULL << 2)
#define HidNpadButton_Y      (1ULL << 3)
#define HidNpadButton_StickL (1ULL << 4)
#define HidNpadButton_StickR (1ULL << 5)
#define HidNpadButton_L      (1ULL << 6)
#define HidNpadButton_R      (1ULL << 7)
#define HidNpadButton_ZL     (1ULL << 8)
#define HidNpadButton_ZR     (1ULL << 9)
#define HidNpadButton_Plus   (1ULL << 10)
#define HidNpadButton_Minus  (1ULL << 11)
#define HidNpadButton_Left   (1ULL << 12)
#define HidNpadButton_Up     (1ULL << 13)
#define HidNpadButton_Right  (1ULL << 14)
#define HidNpadButton_Down   (1ULL << 15)

#endif
