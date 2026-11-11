#ifndef CARAFE_REGISTRY_H
#define CARAFE_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CARAFE_REG_PATH 512
#define CARAFE_REG_NAME 256
#define CARAFE_REG_DATA 1024
#define CARAFE_REG_HIVE_MACHINE 0
#define CARAFE_REG_HIVE_USER 1

#define CARAFE_REG_SZ 1
#define CARAFE_REG_EXPAND_SZ 2
#define CARAFE_REG_BINARY 3
#define CARAFE_REG_DWORD 4
#define CARAFE_REG_MULTI_SZ 7
#define CARAFE_REG_QWORD 11

typedef enum {
    CarafeRegLive,
    CarafeRegChanged,
    CarafeRegDeleted,
    CarafeRegAdded,
} CarafeRegState;

typedef struct {
    bool key;
    char name[CARAFE_REG_NAME];
    uint32_t type;
    uint8_t data[CARAFE_REG_DATA];
    uint32_t size;
    bool truncated;
    CarafeRegState state;
    uint32_t live_type;
    uint8_t live_data[CARAFE_REG_DATA];
    uint32_t live_size;
} CarafeRegEntry;

const char* carafeRegistryHiveName(int hive);

bool carafeRegistryOpen(void);
void carafeRegistryClose(void);

int carafeRegistryRowCount(void);
const char* carafeRegistryRowName(int row);
const char* carafeRegistryRowHint(int row);
int carafeRegistryRowOptionCount(int row);
const char* carafeRegistryRowOptionLabel(int row, int option);
int carafeRegistryRowCurrent(int row);
bool carafeRegistryRowChanged(int row);
void carafeRegistryRowSet(int row, int option);
void carafeRegistryRowReset(int row);
void carafeRegistryRowWhere(int row, char* out, size_t size);

int carafeRegistryList(int hive, const char* path, CarafeRegEntry* out, int max);
int carafeRegistryKeyExists(int hive, const char* path);

bool carafeRegistrySetPending(int hive, const char* path, const char* name, uint32_t type, const uint8_t* data,
    uint32_t size);
bool carafeRegistryDeletePending(int hive, const char* path, const char* name);
bool carafeRegistryDropPending(int hive, const char* path, const char* name);
int carafeRegistryPendingCount(void);
bool carafeRegistryPendingLine(int index, char* name, size_t name_size, char* from, size_t from_size, char* to,
    size_t to_size);
void carafeRegistryUndoPending(void);
void carafeRegistryReset(void);
void carafeRegistryBringBack(void);
void carafeRegistryTrialClear(void);

void carafeRegistryFormat(uint32_t type, const uint8_t* data, uint32_t size, char* out, size_t out_size);
size_t carafeRegistryUtf16(const char* text, uint16_t* out, size_t max);

#endif
