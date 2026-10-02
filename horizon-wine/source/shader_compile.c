#include <stdbool.h>
#include <stdint.h>
#include "shader_alloc_cache.h"

struct nir_shader;
struct nak_compiler;
struct nak_fs_key;
struct nak_shader_bin;

struct nak_shader_bin *__real_nak_compile_shader(struct nir_shader *, bool, const struct nak_compiler *,
                                               uint32_t, const struct nak_fs_key *, bool);
struct nak_shader_bin *__wrap_nak_compile_shader(struct nir_shader *nir, bool dump,
        const struct nak_compiler *nak, uint32_t robust, const struct nak_fs_key *fs, bool task)
{
    wine_nx_shader_alloc_begin();
    struct nak_shader_bin *result = __real_nak_compile_shader(nir, dump, nak, robust, fs, task);
    wine_nx_shader_alloc_end();
    return result;
}
