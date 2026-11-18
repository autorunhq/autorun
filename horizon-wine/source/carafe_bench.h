#ifndef CARAFE_BENCH_H
#define CARAFE_BENCH_H

typedef void (*CarafeBenchTrace)(const char* prefix, const char* text);

void carafeBenchCores(unsigned int mhz, CarafeBenchTrace trace);
void carafeBenchMemory(unsigned int mhz, CarafeBenchTrace trace);
void carafeBenchBranches(unsigned int mhz, CarafeBenchTrace trace);
void carafeBenchCode(unsigned int mhz, CarafeBenchTrace trace);

#endif
