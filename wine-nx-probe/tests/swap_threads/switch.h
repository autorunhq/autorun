#ifndef SWAP_THREADS_SWITCH_H
#define SWAP_THREADS_SWITCH_H
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>

typedef unsigned int Result;
typedef void (*ThreadFunc)( void * );
typedef struct Thread
{
    pthread_t native;
    ThreadFunc entry;
    void *arg;
    int started;
} Thread;
enum { Module_Libnx = 345, LibnxError_OutOfMemory = 2 };
#define MAKERESULT(module,code) ((unsigned int)(module) | ((unsigned int)(code) << 9))
#define R_FAILED(rc) ((rc) != 0)
int threadTlsAlloc( void (*destroy)(void *) );
void *threadTlsGet( int key );
void threadTlsSet( int key, void *value );
#endif
