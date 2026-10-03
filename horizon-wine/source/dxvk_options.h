#ifndef WINE_NX_DXVK_OPTIONS_H
#define WINE_NX_DXVK_OPTIONS_H

#include "launcher_settings.h"

struct dxvk_option_choice
{
    const char *value, *label;
};

struct dxvk_option_range
{
    unsigned int first, end;
};

struct dxvk_option
{
    const char *name, *help, *default_value;
    const struct dxvk_option_choice *choices;
    unsigned int choice_count;
    struct dxvk_option_range official, sarek;
    int advanced, gpl_only;
};

extern const struct dxvk_option dxvk_options[];
extern const unsigned int dxvk_option_count;

unsigned int dxvk_options_version( enum dxvk_source source, const char *version );
int dxvk_option_supported( const struct dxvk_option *option, enum dxvk_source source, unsigned int version );
int dxvk_option_value( const struct launcher_kv *kv, const struct dxvk_option *option );
const char *dxvk_option_default( const struct dxvk_option *option, enum dxvk_source source,
                                 unsigned int version, int hud );
int dxvk_options_apply( const struct launcher_kv *kv, enum dxvk_source source, const char *version,
                        char *config, size_t size );

#endif
