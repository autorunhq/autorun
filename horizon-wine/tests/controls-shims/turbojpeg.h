#ifndef CONTROLS_TEST_TURBOJPEG_H
#define CONTROLS_TEST_TURBOJPEG_H

typedef void *tjhandle;
#define TJPF_BGRA 8
static inline tjhandle tjInitDecompress( void ) { return NULL; }
static inline int tjDecompressHeader3( tjhandle h, const unsigned char *b, unsigned long sz,
                                       int *w, int *ht, int *s, int *c )
{ (void)h; (void)b; (void)sz; (void)w; (void)ht; (void)s; (void)c; return -1; }
static inline int tjDecompress2( tjhandle h, const unsigned char *b, unsigned long sz,
                                 unsigned char *d, int w, int p, int ht, int pf, int fl )
{ (void)h; (void)b; (void)sz; (void)d; (void)w; (void)p; (void)ht; (void)pf; (void)fl; return -1; }
static inline int tjDestroy( tjhandle h ) { (void)h; return 0; }

#endif
