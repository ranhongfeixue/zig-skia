// Embed the Skia ICU data package in a Windows COFF object. The build invokes
// the assembler with the ICU data directory as its working directory, so the
// input stays independent of checkout and runner paths.

#if defined(__i386__)
#define LP_SKIA_ICU_DATA _lp_skia_icudtl_dat
#define LP_SKIA_ICU_DATA_END _lp_skia_icudtl_dat_end
#else
#define LP_SKIA_ICU_DATA lp_skia_icudtl_dat
#define LP_SKIA_ICU_DATA_END lp_skia_icudtl_dat_end
#endif

    .section .rdata
    .balign 16
    .globl LP_SKIA_ICU_DATA
LP_SKIA_ICU_DATA:
    .incbin "icudtl.dat"
    .globl LP_SKIA_ICU_DATA_END
LP_SKIA_ICU_DATA_END:
