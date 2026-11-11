#ifndef CARAFE_ERROR_H
#define CARAFE_ERROR_H

#include <stdbool.h>

void carafeErrorResetStartTick(void) __attribute__((weak));
void carafeErrorException(unsigned int code, const void* address) __attribute__((weak));
void carafeErrorMissingDll(const char* name) __attribute__((weak));
void carafeErrorThreadRefused(void) __attribute__((weak));
bool carafeErrorNative(unsigned int esr, unsigned long pc) __attribute__((weak));
bool carafeErrorRelaunch(const char* own_nro, unsigned int exit_code) __attribute__((weak));
bool carafeErrorRestart(unsigned int exit_code) __attribute__((weak));

#endif
