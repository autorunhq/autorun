#ifndef CARAFE_SETTINGS_H
#define CARAFE_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CARAFE_TEXTS 2
#define CARAFE_SOURCES 24
#define CARAFE_CHORD_MAX 4
#define CARAFE_VALUE_SIZE 64

typedef enum {
    CarafeLanguageRu,
    CarafeLanguageEn,
    CarafeLanguageZhHans,
    CarafeLanguageJa,
    CarafeLanguageKo,
    CarafeLanguageZhHant,
    CarafeLanguageFr,
    CarafeLanguageFrCa,
    CarafeLanguageDe,
    CarafeLanguageIt,
    CarafeLanguageEs,
    CarafeLanguageEs419,
    CarafeLanguageNl,
    CarafeLanguagePt,
    CarafeLanguagePtBr,
    CarafeLanguageCount,
} CarafeLanguage;

typedef enum {
    CarafeFileProgram,
    CarafeFileKeys,
    CarafeFileBox64,
    CarafeFileCount,
} CarafeFile;

typedef enum {
    CarafeWhenAlways,
    CarafeWhenShow,
    CarafeWhenOn,
} CarafeWhenKind;

typedef enum {
    CarafeRowSetting,
    CarafeRowGroup,
} CarafeRowKind;

typedef enum {
    CarafeKindCrash,
    CarafeKindMemory,
    CarafeKindThreads,
    CarafeKindNone,
} CarafeFailure;

typedef struct {
    CarafeWhenKind kind;
    int setting;
    uint32_t values;
} CarafeWhen;

typedef struct {
    const char* text;
    const char* label[CARAFE_TEXTS];
} CarafeValue;

typedef struct {
    const char* id;
    CarafeFile file;
    const char* key;
    bool now;
    int default_index;
    CarafeWhen when;
    uint32_t api;
    const char* name[CARAFE_TEXTS];
    const char* hint[CARAFE_TEXTS];
    CarafeValue* values;
    int value_count;
    int base;
    int start;
    int current;
    char start_text[CARAFE_VALUE_SIZE];
    char current_text[CARAFE_VALUE_SIZE];
} CarafeSetting;

typedef struct {
    CarafeRowKind kind;
    int index;
} CarafeRow;

typedef struct {
    const char* id;
    const char* name[CARAFE_TEXTS];
    CarafeRow* rows;
    int row_count;
} CarafePage;

typedef struct {
    const char* id;
    CarafeWhen when;
    const char* name[CARAFE_TEXTS];
    CarafePage* pages;
    int page_count;
} CarafeGroup;

typedef struct {
    int setting;
    int value;
} CarafeSet;

typedef struct {
    const char* id;
    const char* name[CARAFE_TEXTS];
    const char* hint[CARAFE_TEXTS];
    CarafeSet* sets;
    int set_count;
} CarafePreset;

typedef struct {
    const char* id;
    const char* name[CARAFE_TEXTS];
    CarafeRow* rows;
    int row_count;
    CarafePreset* presets;
    int preset_count;
} CarafeSection;

typedef struct {
    CarafeFailure kind;
    int setting;
    int value;
    const char* why[CARAFE_TEXTS];
} CarafeSuggest;

typedef struct {
    unsigned int code;
    const char* label[CARAFE_TEXTS];
} CarafeKey;

typedef struct {
    uint16_t code[CARAFE_CHORD_MAX];
    int count;
} CarafeChord;

typedef struct {
    char* text;
    uint32_t api;
    CarafeSection* sections;
    int section_count;
    CarafeSetting* settings;
    int setting_count;
    CarafeGroup* groups;
    int group_count;
    CarafeSuggest* suggests;
    int suggest_count;
    CarafeKey* keys;
    int key_count;
    CarafeKey* pads;
    int pad_count;
    CarafeChord key_start[CARAFE_SOURCES];
    CarafeChord key_current[CARAFE_SOURCES];
    CarafeChord pad_start[CARAFE_SOURCES];
    CarafeChord pad_current[CARAFE_SOURCES];
} CarafeCatalog;

extern const char* const carafe_source_names[CARAFE_SOURCES];

CarafeLanguage carafeLanguage(void);
const char* carafeWindowsLocale(void);
int carafePluralForm(int count);
const char* carafeText(const char* const texts[CARAFE_TEXTS]);
CarafeCatalog* carafeCatalogLoad(void);
void carafeCatalogFree(CarafeCatalog* catalog);
int carafeCatalogFind(const CarafeCatalog* catalog, const char* id);
int carafeCatalogFindKey(const CarafeCatalog* catalog, CarafeFile file, const char* key);
bool carafeSettingVisible(const CarafeCatalog* catalog, const CarafeSetting* setting);
bool carafeWhenHolds(const CarafeCatalog* catalog, const CarafeWhen* when, CarafeWhenKind kind);
const char* carafeValueLabel(const CarafeSetting* setting);
bool carafeSettingSet(CarafeCatalog* catalog, int index, int value);
bool carafeSettingPending(const CarafeSetting* setting);
int carafePendingCount(const CarafeCatalog* catalog);
int carafePresetCurrent(const CarafeCatalog* catalog, const CarafeSection* section);
bool carafePresetApply(CarafeCatalog* catalog, const CarafePreset* preset);
bool carafeBindingSet(CarafeCatalog* catalog, bool pad, int source, const CarafeChord* chord);
void carafeChordLabel(const CarafeCatalog* catalog, bool pad, const CarafeChord* chord, char* out, size_t size);
bool carafeUndoPending(CarafeCatalog* catalog);
bool carafeResetAll(CarafeCatalog* catalog, bool live);
bool carafeSaveChanges(void);
bool carafeReadChangesText(char* out, size_t size);
bool carafeWriteChangesText(const char* text);

bool carafeRestartRequested(void);
void carafeRequestRestart(bool trial);
void carafeTrialWrite(bool trial);
bool carafeTrialActive(void);
bool carafeTrialRead(char* out, size_t size);
void carafeTrialClear(void);
unsigned int carafeFailuresAdd(void);
unsigned int carafeFailures(void);
void carafeFailuresClear(void);
bool carafeChangesExist(void);
void carafeSettingsTick(void);
bool carafeGameArgv(char* out, size_t size);
bool carafeOptionsSummary(char* out, size_t size);
bool carafeMeasuring(void);
bool carafeOrchestrating(void);
void carafeSettingsSyncProgram(void) __attribute__((weak));

#endif
