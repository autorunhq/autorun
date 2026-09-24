#include <switch.h>

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winnt.h"
#include "winternl.h"
#include "wine/server.h"
#include "unix_private.h"
#include "horizon_mman.h"
#include "horizon_private.h"

u32 __nx_applet_type = AppletType_Application;
size_t __nx_heap_size = 256 * 1024 * 1024;
unsigned char __attribute__((aligned(16))) __nx_exception_stack[0x10000];
uint64_t __nx_exception_stack_size = sizeof(__nx_exception_stack);

#define PAGE_SIZE 0x1000
#define FILE_ALIGN 0x200
#define PE_FILE_SIZE 0xa00
#define PE_HEADER_SIZE 0x200
#define PE_TEXT_RVA 0x1000
#define PE_TEXT_RAW 0x200
#define PE_TEXT_RAW_SIZE 0x200
#define PE_IDATA_RVA 0x2000
#define PE_IDATA_RAW 0x400
#define PE_IDATA_RAW_SIZE 0x400
#define PE_DATA_RVA 0x3000
#define PE_DATA_RAW 0x800
#define PE_DATA_RAW_SIZE 0x200
#define PE_IMAGE_SIZE 0x4000
#define PE_IMAGE_BASE 0x140000000ULL
#define PE_ENTRY_RESULT 0x574e4954U
#define PE_ENTRY_FAIL 0xbad00001U
#define DATA_HANDLE_RVA PE_DATA_RVA
#define DATA_EVENT_INFO_RVA (PE_DATA_RVA + 8)

static FILE *log_file;
static int failures;

static void log_line( const char *fmt, ... )
{
    va_list args;

    va_start( args, fmt );
    vprintf( fmt, args );
    va_end( args );
    putchar( '\n' );

    if (log_file)
    {
        va_start( args, fmt );
        vfprintf( log_file, fmt, args );
        va_end( args );
        fputc( '\n', log_file );
        fflush( log_file );
    }
    consoleUpdate( NULL );
}

static int check_bool( const char *name, int ok )
{
    if (ok)
    {
        log_line( "[OK] %s", name );
        return 1;
    }

    log_line( "[FAIL] %s", name );
    failures++;
    return 0;
}

static int check_status( const char *name, unsigned int status )
{
    if (status == STATUS_SUCCESS)
    {
        log_line( "[OK] %s", name );
        return 1;
    }

    log_line( "[FAIL] %s status=%08x", name, status );
    failures++;
    return 0;
}

static NTSTATUS WINAPI import_NtCreateEvent( HANDLE *handle, ACCESS_MASK access,
                                             const OBJECT_ATTRIBUTES *attr,
                                             EVENT_TYPE type, BOOLEAN state )
{
    NTSTATUS status;

    log_line( "[IMP] NtCreateEvent enter access=0x%x type=%u state=%u", access, type, state );
    status = NtCreateEvent( handle, access, attr, type, state );
    log_line( "[IMP] NtCreateEvent ret=0x%08x handle=%p", status, handle ? *handle : 0 );
    return status;
}

static NTSTATUS WINAPI import_NtSetEvent( HANDLE handle, LONG *prev_state )
{
    NTSTATUS status;
    LONG local_prev = -1;

    log_line( "[IMP] NtSetEvent enter handle=%p", handle );
    status = NtSetEvent( handle, prev_state ? prev_state : &local_prev );
    log_line( "[IMP] NtSetEvent ret=0x%08x prev=%ld", status, prev_state ? *prev_state : local_prev );
    return status;
}

static NTSTATUS WINAPI import_NtQueryEvent( HANDLE handle, EVENT_INFORMATION_CLASS class,
                                            void *info, ULONG len, ULONG *ret_len )
{
    NTSTATUS status;
    EVENT_BASIC_INFORMATION *basic = info;

    log_line( "[IMP] NtQueryEvent enter handle=%p class=%u len=%u", handle, class, len );
    status = NtQueryEvent( handle, class, info, len, ret_len );
    if (!status && basic)
        log_line( "[IMP] NtQueryEvent ret=0x%08x type=%d state=%ld", status,
                  basic->EventType, basic->EventState );
    else
        log_line( "[IMP] NtQueryEvent ret=0x%08x", status );
    return status;
}

static uint32_t align_up_u32( uint32_t value, uint32_t align )
{
    return (value + align - 1) & ~(align - 1);
}

static void put16( unsigned char *ptr, uint16_t value )
{
    ptr[0] = value & 0xff;
    ptr[1] = value >> 8;
}

static void put32( unsigned char *ptr, uint32_t value )
{
    ptr[0] = value & 0xff;
    ptr[1] = (value >> 8) & 0xff;
    ptr[2] = (value >> 16) & 0xff;
    ptr[3] = value >> 24;
}

static void put64( unsigned char *ptr, uint64_t value )
{
    put32( ptr, value & 0xffffffffU );
    put32( ptr + 4, value >> 32 );
}

static uint16_t get16( const unsigned char *ptr )
{
    return ptr[0] | ((uint16_t)ptr[1] << 8);
}

static uint32_t get32( const unsigned char *ptr )
{
    return ptr[0] | ((uint32_t)ptr[1] << 8) | ((uint32_t)ptr[2] << 16) | ((uint32_t)ptr[3] << 24);
}

static uint64_t get64( const unsigned char *ptr )
{
    return get32( ptr ) | ((uint64_t)get32( ptr + 4 ) << 32);
}

static uint32_t arm64_movz_w( unsigned int reg, uint16_t imm, unsigned int shift )
{
    return 0x52800000U | (((shift / 16) & 3) << 21) | ((uint32_t)imm << 5) | (reg & 31);
}

static uint32_t arm64_movk_w( unsigned int reg, uint16_t imm, unsigned int shift )
{
    return 0x72800000U | (((shift / 16) & 3) << 21) | ((uint32_t)imm << 5) | (reg & 31);
}

static uint32_t arm64_movz_x( unsigned int reg, uint16_t imm, unsigned int shift )
{
    return 0xd2800000U | (((shift / 16) & 3) << 21) | ((uint32_t)imm << 5) | (reg & 31);
}

struct code_builder
{
    unsigned char *text;
    uint32_t text_rva;
    uint32_t pos;
    uint32_t status_branches[8];
    unsigned int status_branch_count;
    uint32_t fail_branch;
};

static void emit32( struct code_builder *code, uint32_t insn )
{
    put32( code->text + code->pos, insn );
    code->pos += 4;
}

static void emit_adrp_add( struct code_builder *code, unsigned int reg, uint32_t target_rva )
{
    int32_t pc_page = (code->text_rva + code->pos) & ~0xfff;
    int32_t target_page = target_rva & ~0xfff;
    int32_t page_delta = (target_page - pc_page) / 0x1000;
    uint32_t immlo = page_delta & 3;
    uint32_t immhi = (page_delta >> 2) & 0x7ffff;

    emit32( code, 0x90000000U | (immlo << 29) | (immhi << 5) | (reg & 31) );
    emit32( code, 0x91000000U | ((target_rva & 0xfff) << 10) | ((reg & 31) << 5) | (reg & 31) );
}

static void emit_call_iat( struct code_builder *code, uint32_t iat_rva )
{
    emit_adrp_add( code, 16, iat_rva );
    emit32( code, 0xf9400210U );
    emit32( code, 0xd63f0200U );
}

static void emit_status_branch( struct code_builder *code )
{
    code->status_branches[code->status_branch_count++] = code->text_rva + code->pos;
    emit32( code, 0x35000000U );
}

static void emit_epilogue_return( struct code_builder *code )
{
    emit32( code, 0xa8c17bfdU );
    emit32( code, 0xd65f03c0U );
}

static void patch_cond_branch( unsigned char *text, uint32_t insn_rva, uint32_t target_rva )
{
    uint32_t offset = (insn_rva - PE_TEXT_RVA);
    uint32_t insn = get32( text + offset );
    int32_t imm19 = ((int32_t)target_rva - (int32_t)insn_rva) / 4;

    insn &= ~(0x7ffffU << 5);
    insn |= ((uint32_t)imm19 & 0x7ffffU) << 5;
    put32( text + offset, insn );
}

static void build_pe_code( unsigned char *text, uint32_t iat_rva )
{
    struct code_builder code;
    uint32_t ret_status_rva;
    uint32_t fail_rva;
    unsigned int i;

    memset( &code, 0, sizeof(code) );
    code.text = text;
    code.text_rva = PE_TEXT_RVA;

    emit32( &code, 0xa9bf7bfdU );
    emit32( &code, 0x910003fdU );

    emit_adrp_add( &code, 0, DATA_HANDLE_RVA );
    emit32( &code, arm64_movz_w( 1, EVENT_ALL_ACCESS & 0xffff, 0 ) );
    emit32( &code, arm64_movk_w( 1, EVENT_ALL_ACCESS >> 16, 16 ) );
    emit32( &code, arm64_movz_x( 2, 0, 0 ) );
    emit32( &code, arm64_movz_w( 3, SynchronizationEvent, 0 ) );
    emit32( &code, arm64_movz_w( 4, 0, 0 ) );
    emit_call_iat( &code, iat_rva );
    emit_status_branch( &code );

    emit_adrp_add( &code, 0, DATA_HANDLE_RVA );
    emit32( &code, 0xf9400000U );
    emit32( &code, arm64_movz_x( 1, 0, 0 ) );
    emit_call_iat( &code, iat_rva + 8 );
    emit_status_branch( &code );

    emit_adrp_add( &code, 0, DATA_HANDLE_RVA );
    emit32( &code, 0xf9400000U );
    emit32( &code, arm64_movz_w( 1, EventBasicInformation, 0 ) );
    emit_adrp_add( &code, 2, DATA_EVENT_INFO_RVA );
    emit32( &code, arm64_movz_w( 3, sizeof(EVENT_BASIC_INFORMATION), 0 ) );
    emit32( &code, arm64_movz_x( 4, 0, 0 ) );
    emit_call_iat( &code, iat_rva + 16 );
    emit_status_branch( &code );

    emit_adrp_add( &code, 1, DATA_EVENT_INFO_RVA );
    emit32( &code, 0xb9400420U );
    emit32( &code, 0x7100041fU );
    code.fail_branch = code.text_rva + code.pos;
    emit32( &code, 0x54000001U );

    emit32( &code, arm64_movz_w( 0, PE_ENTRY_RESULT & 0xffff, 0 ) );
    emit32( &code, arm64_movk_w( 0, PE_ENTRY_RESULT >> 16, 16 ) );
    emit_epilogue_return( &code );

    fail_rva = code.text_rva + code.pos;
    emit32( &code, arm64_movz_w( 0, PE_ENTRY_FAIL & 0xffff, 0 ) );
    emit32( &code, arm64_movk_w( 0, PE_ENTRY_FAIL >> 16, 16 ) );
    emit_epilogue_return( &code );

    ret_status_rva = code.text_rva + code.pos;
    emit_epilogue_return( &code );

    for (i = 0; i < code.status_branch_count; i++)
        patch_cond_branch( text, code.status_branches[i], ret_status_rva );
    patch_cond_branch( text, code.fail_branch, fail_rva );
}

static uint32_t write_import_name( unsigned char *idata, uint32_t offset, const char *name )
{
    uint32_t rva = PE_IDATA_RVA + offset;
    size_t len = strlen( name ) + 1;

    put16( idata + offset, 0 );
    memcpy( idata + offset + 2, name, len );
    return rva;
}

static void build_imports( unsigned char *pe, uint32_t *iat_rva )
{
    unsigned char *idata = pe + PE_IDATA_RAW;
    uint32_t ilt_offset = 0x40;
    uint32_t iat_offset = 0x80;
    uint32_t dll_offset = 0xc0;
    uint32_t name_offset = 0xd0;
    uint32_t create_name_rva;
    uint32_t set_name_rva;
    uint32_t query_name_rva;

    memset( idata, 0, PE_IDATA_RAW_SIZE );

    create_name_rva = write_import_name( idata, name_offset, "NtCreateEvent" );
    name_offset = align_up_u32( name_offset + 2 + strlen( "NtCreateEvent" ) + 1, 2 );
    set_name_rva = write_import_name( idata, name_offset, "NtSetEvent" );
    name_offset = align_up_u32( name_offset + 2 + strlen( "NtSetEvent" ) + 1, 2 );
    query_name_rva = write_import_name( idata, name_offset, "NtQueryEvent" );

    memcpy( idata + dll_offset, "ntdll.dll", sizeof("ntdll.dll") );

    put32( idata, PE_IDATA_RVA + ilt_offset );
    put32( idata + 12, PE_IDATA_RVA + dll_offset );
    put32( idata + 16, PE_IDATA_RVA + iat_offset );

    put64( idata + ilt_offset, create_name_rva );
    put64( idata + ilt_offset + 8, set_name_rva );
    put64( idata + ilt_offset + 16, query_name_rva );

    put64( idata + iat_offset, create_name_rva );
    put64( idata + iat_offset + 8, set_name_rva );
    put64( idata + iat_offset + 16, query_name_rva );

    *iat_rva = PE_IDATA_RVA + iat_offset;
}

static void build_arm64_pe_with_ntdll_imports( unsigned char pe[PE_FILE_SIZE] )
{
    const size_t pe_offset = 0x80;
    const size_t coff = pe_offset + 4;
    const size_t opt = coff + 20;
    const size_t section = opt + 0xf0;
    uint32_t iat_rva;

    memset( pe, 0, PE_FILE_SIZE );

    put16( pe, 0x5a4d );
    put32( pe + 0x3c, pe_offset );

    put32( pe + pe_offset, 0x00004550 );
    put16( pe + coff, 0xaa64 );
    put16( pe + coff + 2, 3 );
    put16( pe + coff + 16, 0xf0 );
    put16( pe + coff + 18, 0x0022 );

    put16( pe + opt, 0x20b );
    pe[opt + 2] = 1;
    put32( pe + opt + 4, PE_TEXT_RAW_SIZE );
    put32( pe + opt + 8, PE_IDATA_RAW_SIZE + PE_DATA_RAW_SIZE );
    put32( pe + opt + 16, PE_TEXT_RVA );
    put32( pe + opt + 20, PE_TEXT_RVA );
    put64( pe + opt + 24, PE_IMAGE_BASE );
    put32( pe + opt + 32, PAGE_SIZE );
    put32( pe + opt + 36, FILE_ALIGN );
    put16( pe + opt + 40, 6 );
    put16( pe + opt + 48, 6 );
    put32( pe + opt + 56, PE_IMAGE_SIZE );
    put32( pe + opt + 60, PE_HEADER_SIZE );
    put16( pe + opt + 68, 3 );
    put64( pe + opt + 72, 0x100000 );
    put64( pe + opt + 80, 0x1000 );
    put64( pe + opt + 88, 0x100000 );
    put64( pe + opt + 96, 0x1000 );
    put32( pe + opt + 108, 16 );
    put32( pe + opt + 112 + 8, PE_IDATA_RVA );
    put32( pe + opt + 112 + 12, PE_IDATA_RAW_SIZE );

    memcpy( pe + section, ".text", 5 );
    put32( pe + section + 8, PE_TEXT_RAW_SIZE );
    put32( pe + section + 12, PE_TEXT_RVA );
    put32( pe + section + 16, PE_TEXT_RAW_SIZE );
    put32( pe + section + 20, PE_TEXT_RAW );
    put32( pe + section + 36, 0x60000020 );

    memcpy( pe + section + 40, ".idata", 7 );
    put32( pe + section + 48, PE_IDATA_RAW_SIZE );
    put32( pe + section + 52, PE_IDATA_RVA );
    put32( pe + section + 56, PE_IDATA_RAW_SIZE );
    put32( pe + section + 60, PE_IDATA_RAW );
    put32( pe + section + 76, 0xc0000040 );

    memcpy( pe + section + 80, ".data", 6 );
    put32( pe + section + 88, PE_DATA_RAW_SIZE );
    put32( pe + section + 92, PE_DATA_RVA );
    put32( pe + section + 96, PE_DATA_RAW_SIZE );
    put32( pe + section + 100, PE_DATA_RAW );
    put32( pe + section + 116, 0xc0000040 );

    build_imports( pe, &iat_rva );
    build_pe_code( pe + PE_TEXT_RAW, iat_rva );
}

static int rva_to_file_offset( const unsigned char *pe, size_t pe_size, uint32_t rva, uint32_t *file_offset )
{
    uint32_t pe_offset = get32( pe + 0x3c );
    const unsigned char *coff = pe + pe_offset + 4;
    const unsigned char *section = coff + 20 + get16( coff + 16 );
    unsigned int section_count = get16( coff + 2 );
    unsigned int i;

    if (rva < PE_HEADER_SIZE)
    {
        *file_offset = rva;
        return rva < pe_size;
    }

    for (i = 0; i < section_count; i++)
    {
        const unsigned char *hdr = section + i * 40;
        uint32_t virtual_size = get32( hdr + 8 );
        uint32_t virtual_address = get32( hdr + 12 );
        uint32_t raw_size = get32( hdr + 16 );
        uint32_t raw_ptr = get32( hdr + 20 );
        uint32_t span = virtual_size > raw_size ? virtual_size : raw_size;

        if (rva >= virtual_address && rva < virtual_address + span)
        {
            *file_offset = raw_ptr + (rva - virtual_address);
            return *file_offset < pe_size;
        }
    }

    return 0;
}

static void *resolve_ntdll_import( const char *dll, const char *name )
{
    if (strcasecmp( dll, "ntdll.dll" )) return NULL;
    if (!strcmp( name, "NtCreateEvent" )) return import_NtCreateEvent;
    if (!strcmp( name, "NtSetEvent" )) return import_NtSetEvent;
    if (!strcmp( name, "NtQueryEvent" )) return import_NtQueryEvent;
    return NULL;
}

static int resolve_imports( unsigned char *image, const unsigned char *pe, size_t pe_size,
                            uint32_t import_rva, uint32_t import_size )
{
    uint32_t desc_rva = import_rva;

    if (!import_rva || !import_size) return 1;

    for (;;)
    {
        uint32_t desc_offset;
        uint32_t ilt_rva;
        uint32_t name_rva;
        uint32_t iat_rva;
        uint32_t name_offset;
        const char *dll_name;
        unsigned int index;

        if (!rva_to_file_offset( pe, pe_size, desc_rva, &desc_offset ) || desc_offset + 20 > pe_size) return 0;
        ilt_rva = get32( pe + desc_offset );
        name_rva = get32( pe + desc_offset + 12 );
        iat_rva = get32( pe + desc_offset + 16 );
        if (!ilt_rva && !name_rva && !iat_rva) return 1;
        if (!rva_to_file_offset( pe, pe_size, name_rva, &name_offset )) return 0;
        dll_name = (const char *)pe + name_offset;

        for (index = 0;; index++)
        {
            uint64_t thunk = get64( image + ilt_rva + index * 8 );
            uint32_t thunk_offset;
            const char *func_name;
            void *func;

            if (!thunk) break;
            if (thunk & 0x8000000000000000ULL) return 0;
            if (!rva_to_file_offset( pe, pe_size, thunk, &thunk_offset ) || thunk_offset + 2 >= pe_size) return 0;
            func_name = (const char *)pe + thunk_offset + 2;
            func = resolve_ntdll_import( dll_name, func_name );
            if (!func)
            {
                log_line( "[FAIL] unresolved import %s!%s", dll_name, func_name );
                return 0;
            }
            put64( image + iat_rva + index * 8, (uint64_t)(uintptr_t)func );
            log_line( "[PE] import %s!%s -> %p", dll_name, func_name, func );
        }
        desc_rva += 20;
    }
}

static int protect_sections( unsigned char *image, const unsigned char *section, unsigned int section_count )
{
    unsigned int i;

    for (i = 0; i < section_count; i++)
    {
        const unsigned char *hdr = section + i * 40;
        uint32_t virtual_size = get32( hdr + 8 );
        uint32_t virtual_address = get32( hdr + 12 );
        uint32_t raw_size = get32( hdr + 16 );
        uint32_t characteristics = get32( hdr + 36 );
        uint32_t size = align_up_u32( virtual_size > raw_size ? virtual_size : raw_size, PAGE_SIZE );
        int prot = PROT_READ;

        if (characteristics & 0x20000000) prot |= PROT_EXEC;
        if ((characteristics & 0x80000000) && !(prot & PROT_EXEC)) prot |= PROT_WRITE;
        if (!size) continue;

        if (horizon_mprotect( image + virtual_address, size, prot ) == -1)
        {
            log_line( "[FAIL] mprotect %.8s errno=%d", hdr, errno );
            return 0;
        }
        log_line( "[PE] protect %.8s prot=0x%x", hdr, prot );
    }
    return 1;
}

static uint32_t run_pe_image( const unsigned char *pe, size_t pe_size )
{
    uint32_t pe_offset;
    const unsigned char *coff;
    const unsigned char *opt;
    const unsigned char *section;
    unsigned int section_count;
    uint16_t optional_size;
    uint32_t entry_rva;
    uint32_t image_size;
    uint32_t header_size;
    uint32_t import_rva;
    uint32_t import_size;
    unsigned char *image;
    unsigned int i;
    uint32_t (*entry)(void);
    uint32_t result;

    if (pe_size < PE_HEADER_SIZE || get16( pe ) != 0x5a4d) return 0xffffffffU;
    pe_offset = get32( pe + 0x3c );
    if (pe_offset + 4 + 20 > pe_size || get32( pe + pe_offset ) != 0x00004550) return 0xfffffffeU;

    coff = pe + pe_offset + 4;
    optional_size = get16( coff + 16 );
    opt = coff + 20;
    section = opt + optional_size;
    section_count = get16( coff + 2 );

    if (get16( coff ) != 0xaa64 || get16( opt ) != 0x20b) return 0xfffffffdU;
    if (optional_size < 0xf0 || section + section_count * 40 > pe + pe_size) return 0xfffffffcU;

    entry_rva = get32( opt + 16 );
    image_size = get32( opt + 56 );
    header_size = get32( opt + 60 );
    import_rva = get32( opt + 112 + 8 );
    import_size = get32( opt + 112 + 12 );

    log_line( "[PE] machine=ARM64 sections=%u image_size=0x%x entry_rva=0x%x import=0x%x/0x%x",
              section_count, image_size, entry_rva, import_rva, import_size );

    image = horizon_mmap( NULL, image_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0 );
    if (image == MAP_FAILED)
    {
        log_line( "[FAIL] mmap image errno=%d", errno );
        failures++;
        return 0xfffffffbU;
    }

    memset( image, 0, image_size );
    memcpy( image, pe, header_size < pe_size ? header_size : pe_size );

    for (i = 0; i < section_count; i++)
    {
        const unsigned char *hdr = section + i * 40;
        uint32_t virtual_size = get32( hdr + 8 );
        uint32_t virtual_address = get32( hdr + 12 );
        uint32_t raw_size = get32( hdr + 16 );
        uint32_t raw_ptr = get32( hdr + 20 );

        if (virtual_address + virtual_size > image_size ||
            virtual_address + raw_size > image_size ||
            raw_ptr + raw_size > pe_size)
        {
            log_line( "[FAIL] invalid section %u", i );
            failures++;
            horizon_munmap( image, image_size );
            return 0xfffffffaU;
        }

        memcpy( image + virtual_address, pe + raw_ptr, raw_size );
        log_line( "[PE] section %.8s va=0x%x raw=0x%x", hdr, virtual_address, raw_size );
    }

    if (!resolve_imports( image, pe, pe_size, import_rva, import_size ))
    {
        failures++;
        horizon_munmap( image, image_size );
        return 0xfffffff9U;
    }
    check_bool( "PE imports resolved", 1 );

    if (!protect_sections( image, section, section_count ))
    {
        failures++;
        horizon_munmap( image, image_size );
        return 0xfffffff8U;
    }

    entry = (uint32_t (*)(void))(void *)(image + entry_rva);
    log_line( "[PE] entering entrypoint %p", entry );
    result = entry();
    log_line( "[PE] entrypoint returned 0x%08x", result );
    check_bool( "PE imported ntdll calls returned expected value", result == PE_ENTRY_RESULT );

    horizon_munmap( image, image_size );
    return result;
}

static unsigned int smoke_init_first_thread(void)
{
    NtCurrentTeb();
    struct thread_data *thread_data = get_thread_data();
    USHORT machines[8] = { 0 };
    obj_handle_t version = 0;
    int control_fd;
    int reply_pipe[2] = {-1, -1};
    int wait_pipe[2] = {-1, -1};
    unsigned int status;

    control_fd = horizon_server_connect();
    thread_data->request_fd = wine_server_receive_fd( &version );
    if (version != SERVER_PROTOCOL_VERSION)
    {
        log_line( "[FAIL] protocol version got=%u expected=%u", version, SERVER_PROTOCOL_VERSION );
        failures++;
        return STATUS_REVISION_MISMATCH;
    }

    if (horizon_pipe( reply_pipe ) == -1 || horizon_pipe( wait_pipe ) == -1)
    {
        log_line( "[FAIL] bootstrap pipe errno=%d", errno );
        failures++;
        return STATUS_UNSUCCESSFUL;
    }

    thread_data->reply_fd = reply_pipe[0];
    thread_data->wait_fd[0] = wait_pipe[0];
    thread_data->wait_fd[1] = wait_pipe[1];

    wine_server_send_fd( reply_pipe[1] );
    wine_server_send_fd( wait_pipe[1] );

    SERVER_START_REQ( init_first_thread )
    {
        req->unix_pid = 1;
        req->unix_tid = 1;
        req->reply_fd = reply_pipe[1];
        req->wait_fd = wait_pipe[1];
        req->debug_level = 0;
        req->page_size = 4096;
        wine_server_set_reply( req, machines, sizeof(machines) );
        status = wine_server_call( req );
        if (!status)
        {
            check_bool( "init_first_thread machine", wine_server_reply_size( reply ) >= sizeof(USHORT) &&
                        machines[0] == IMAGE_FILE_MACHINE_ARM64 );
            log_line( "[INFO] init pid=%u tid=%u session=%u", reply->pid, reply->tid, reply->session_id );
        }
    }
    SERVER_END_REQ;

    close( reply_pipe[1] );
    close( control_fd );
    return status;
}

static unsigned int smoke_init_process_done(void)
{
    unsigned int status;

    SERVER_START_REQ( init_process_done )
    {
        req->teb = wine_server_client_ptr( NtCurrentTeb() );
        req->peb = wine_server_client_ptr( NtCurrentTeb()->Peb );
        status = wine_server_call( req );
        if (!status) log_line( "[INFO] init_process_done suspend=%d", reply->suspend );
    }
    SERVER_END_REQ;

    return status;
}

static void run_direct_ntdll_smoke(void)
{
    HANDLE handle = 0;
    EVENT_BASIC_INFORMATION info;
    NTSTATUS status;

    memset( &info, 0, sizeof(info) );

    log_line( "[HOST] direct NtCreateEvent/NtSetEvent/NtQueryEvent smoke" );
    status = NtCreateEvent( &handle, EVENT_ALL_ACCESS, NULL, SynchronizationEvent, FALSE );
    if (!check_status( "host NtCreateEvent", status )) return;

    status = NtSetEvent( handle, NULL );
    if (!check_status( "host NtSetEvent", status )) goto done;

    status = NtQueryEvent( handle, EventBasicInformation, &info, sizeof(info), NULL );
    if (check_status( "host NtQueryEvent", status ))
    {
        check_bool( "host NtQueryEvent type", info.EventType == SynchronizationEvent );
        check_bool( "host NtQueryEvent state", info.EventState == 1 );
    }

done:
    (void)handle;
}

static void park_forever(void)
{
    log_line( "[EXIT] parked after summary; close the application from HOME" );
    for (;;) svcSleepThread( 1000000000LL );
}

int main(int argc, char **argv)
{
    unsigned char pe[PE_FILE_SIZE];
    FILE *pe_file;
    uint32_t result;
    unsigned int status;

    (void)argc;
    (void)argv;

    consoleInit( NULL );
    mkdir( "sdmc:/switch", 0777 );
    mkdir( "sdmc:/switch/wine-nx-probe", 0777 );
    log_file = fopen( "sdmc:/switch/wine-nx-probe/pe-ntdll-smoke.log", "w" );

    log_line( "wine-nx-pe-ntdll-smoke: ARM64 PE imports ntdll.dll and calls Wine ntdll" );
    build_arm64_pe_with_ntdll_imports( pe );

    pe_file = fopen( "sdmc:/switch/wine-nx-probe/hello-arm64-ntdll.exe", "wb" );
    if (pe_file)
    {
        fwrite( pe, 1, sizeof(pe), pe_file );
        fclose( pe_file );
        log_line( "[OK] wrote hello-arm64-ntdll.exe" );
    }
    else log_line( "[INFO] could not write PE file errno=%d", errno );

    check_bool( "MZ header", get16( pe ) == 0x5a4d );
    check_bool( "PE signature", get32( pe + get32( pe + 0x3c ) ) == 0x00004550 );

    status = smoke_init_first_thread();
    if (check_status( "init_first_thread", status ))
        check_status( "init_process_done", smoke_init_process_done() );

    run_direct_ntdll_smoke();

    result = run_pe_image( pe, sizeof(pe) );
    log_line( "[PE] entry result=0x%08x", result );
    log_line( "SUMMARY failures=%d overall=%s", failures, failures ? "FAIL" : "OK" );
    park_forever();
    return failures ? 1 : 0;
}
