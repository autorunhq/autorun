/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include "windef.h"
#include "winternl.h"
#include "../../dlls/winebox64/cpuid.h"

int main(void)
{
    /* What SystemEmulationProcessorInformation returns on an ARM64 host. */
    SYSTEM_CPU_INFORMATION info = { .ProcessorArchitecture = PROCESSOR_ARCHITECTURE_ARM,
                                    .ProcessorLevel = 0xd07, .ProcessorRevision = 0x0101,
                                    .MaximumProcessors = 3 };
    static const struct { unsigned int edx_bit; UINT feature; } cpuid_features[] =
    {
        { 4, PF_RDTSC_INSTRUCTION_AVAILABLE },
        { 8, PF_COMPARE_EXCHANGE_DOUBLE },
        { 23, PF_MMX_INSTRUCTIONS_AVAILABLE },
        { 25, PF_XMMI_INSTRUCTIONS_AVAILABLE },
        { 26, PF_XMMI64_INSTRUCTIONS_AVAILABLE },
    };
    static const struct { unsigned int ecx_bit; UINT feature; } extended_features[] =
    {
        { 0, PF_SSE3_INSTRUCTIONS_AVAILABLE },
        { 9, PF_SSSE3_INSTRUCTIONS_AVAILABLE },
        { 19, PF_SSE4_1_INSTRUCTIONS_AVAILABLE },
        { 20, PF_SSE4_2_INSTRUCTIONS_AVAILABLE },
    };
    unsigned int i;

    winebox64_x86_processor_information( &info );
    assert( info.ProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL );
    /* Wine's i386 formula: family 15, revision (model << 8) | stepping. */
    assert( info.ProcessorLevel == 15 && info.ProcessorRevision == 0x0209 );
    assert( info.MaximumProcessors == 3 ); /* the host's count is kept */
    assert( info.ProcessorFeatureBits == (0x00000275 | 0x2 | 0x80 | 0x100 | 0x42800 | 0x10000 | 0x80000 | 0x08000000) );

    for (i = 0; i < sizeof(cpuid_features) / sizeof(cpuid_features[0]); i++)
        assert( !!(WINEBOX64_CPUID_EDX & (1u << cpuid_features[i].edx_bit)) ==
                !!winebox64_x86_feature_present( cpuid_features[i].feature ) );
    assert( WINEBOX64_CPUID_ECX == 0x00980201 );
    for (i = 0; i < sizeof(extended_features) / sizeof(extended_features[0]); i++)
        assert( !!(WINEBOX64_CPUID_ECX & (1u << extended_features[i].ecx_bit)) ==
                !!winebox64_x86_feature_present( extended_features[i].feature ) );
    assert( !winebox64_x86_feature_present( PF_AVX_INSTRUCTIONS_AVAILABLE ) );
    assert( !winebox64_x86_feature_present( PF_AVX2_INSTRUCTIONS_AVAILABLE ) );
    assert( !winebox64_x86_feature_present( PF_XSAVE_ENABLED ) );
    assert( !winebox64_x86_feature_present( PF_RDRAND_INSTRUCTION_AVAILABLE ) );
    assert( !winebox64_x86_feature_present( PF_NX_ENABLED ) );
    assert( !(WINEBOX64_CPUID_EDX & (1u << 28)) ); /* no HTT */
    puts( "winebox64 CPU identity: CPUID, feature answers and x86 processor information agree" );
    return 0;
}
