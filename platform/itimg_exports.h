/*************************************************************
 * platform/itimg_exports.h
 * C++ declarations for the 1992 Midway MASM routines and data
 *************************************************************/
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Asm-side global variables ---- */
extern unsigned int   shim_ebx, shim_ecx, shim_edx;
extern unsigned short shim_keycode;
extern int            shim_zf;
extern struct SDL_Color g_palette[256];
extern void          *img_p;
extern unsigned int   imgcnt;
extern int            ilselected;
extern void          *pal_p;
extern unsigned int   palcnt;
extern int            plselected;
extern unsigned int   seqcnt;
extern unsigned int   scrcnt;
extern unsigned int   damcnt;
extern unsigned int   fileversion;
extern char           fpath_s[64];
extern char           fname_s[13];
extern char           fnametmp_s[13];
/* Second image list globals (for dual-list / Tab switching) */
extern void          *img2_p;
extern unsigned int   img2cnt;
extern int            il2selected;
extern unsigned int   il1stprt;
extern unsigned int   il21stprt;

/* ---- Asm-side internal subroutines ---- */
void ilst_duplicate(void);
void plst_histogram(void);
void imgtool_clearall(void);   /* cdecl-safe wrapper around asm img_clearall */
void imgtool_addnewpal(void);
void img_load(void);
void img_save(void);
void loadlbm(void);
void savelbm(void);
void loadtga(void);
void savetga(void);
void* img_alloc(void);
void* imgtool_img_pttbladd(int img_idx);   /* cdecl-safe wrapper, allocates PTTBL */
void* imgtool_img_alloc(void);             /* cdecl-safe wrapper around img_alloc */
void* imgtool_pal_alloc(void);             /* cdecl-safe wrapper around pal_alloc */
void* imgtool_mem_alloc(unsigned int size); /* cdecl-safe wrapper around ASM mem_alloc */
void mem_free(void* ptr);
void* mem_duplicate(void* ptr);
void ilst_setidfmnxtlst(void);
void ilst_nxtlst(void);
void plst_merge(void);

#ifdef __cplusplus
}
#endif

/* ---- Linker directives to map C-symbols to ASM COFF names ---- */
#ifdef _MSC_VER
#pragma comment(linker, "/alternatename:_img2_p=img2_p")
#pragma comment(linker, "/alternatename:_img2cnt=img2cnt")
#pragma comment(linker, "/alternatename:_il2selected=il2selected")
#pragma comment(linker, "/alternatename:_il1stprt=il1stprt")
#pragma comment(linker, "/alternatename:_il21stprt=il21stprt")
#pragma comment(linker, "/alternatename:_img_p=img_p")
#pragma comment(linker, "/alternatename:_imgcnt=imgcnt")
#pragma comment(linker, "/alternatename:_ilselected=ilselected")
#pragma comment(linker, "/alternatename:_pal_p=pal_p")
#pragma comment(linker, "/alternatename:_palcnt=palcnt")
#pragma comment(linker, "/alternatename:_plselected=plselected")
#pragma comment(linker, "/alternatename:_seqcnt=seqcnt")
#pragma comment(linker, "/alternatename:_scrcnt=scrcnt")
#pragma comment(linker, "/alternatename:_damcnt=damcnt")
#pragma comment(linker, "/alternatename:_fileversion=fileversion")
#pragma comment(linker, "/alternatename:_fpath_s=fpath_s")
#pragma comment(linker, "/alternatename:_fname_s=fname_s")
#pragma comment(linker, "/alternatename:_fnametmp_s=fnametmp_s")
#pragma comment(linker, "/alternatename:_ilst_duplicate=ilst_duplicate")
#pragma comment(linker, "/alternatename:_plst_histogram=plst_histogram")
#pragma comment(linker, "/alternatename:_imgtool_clearall=imgtool_clearall")
#pragma comment(linker, "/alternatename:_imgtool_addnewpal=imgtool_addnewpal")
#pragma comment(linker, "/alternatename:_img_load=img_load")
#pragma comment(linker, "/alternatename:_img_save=img_save")
#pragma comment(linker, "/alternatename:_loadlbm=loadlbm")
#pragma comment(linker, "/alternatename:_savelbm=savelbm")
#pragma comment(linker, "/alternatename:_loadtga=loadtga")
#pragma comment(linker, "/alternatename:_savetga=savetga")
#pragma comment(linker, "/alternatename:_img_alloc=img_alloc")
#pragma comment(linker, "/alternatename:_imgtool_img_pttbladd=imgtool_img_pttbladd")
#pragma comment(linker, "/alternatename:_imgtool_img_alloc=imgtool_img_alloc")
#pragma comment(linker, "/alternatename:_imgtool_pal_alloc=imgtool_pal_alloc")
#pragma comment(linker, "/alternatename:_imgtool_mem_alloc=imgtool_mem_alloc")
#pragma comment(linker, "/alternatename:_mem_free=mem_free")
#pragma comment(linker, "/alternatename:_mem_duplicate=mem_duplicate")
#pragma comment(linker, "/alternatename:_ilst_setidfmnxtlst=ilst_setidfmnxtlst")
#pragma comment(linker, "/alternatename:_ilst_nxtlst=ilst_nxtlst")
#pragma comment(linker, "/alternatename:_plst_merge=plst_merge")
#endif