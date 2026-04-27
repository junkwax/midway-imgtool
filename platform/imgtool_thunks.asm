;*************************************************************
;* platform/imgtool_thunks.asm
;* MASM thunks for ImGui overlay → asm world calls that need
;* register-arg / EAX-return marshaling outside the standard
;* zero-arg/zero-ret pattern.
;*************************************************************
    .386P
    .model  flat, syscall
    option  casemap:none

    include wmpstruc.inc

    .code
    externdef   pal_alloc:near      ; itimg.asm: ()->EAX=*PAL or 0
    externdef   mem_alloc:near      ; itos.asm:  EAX=len -> EAX=*mem or 0
    externdef   img_clearall:near   ; itimg.asm: () — but clobbers EBX/ESI/EDI/EBP
    externdef   img_alloc:near      ; itimg.asm: ()->EAX=*IMG or 0

;*************************************************************
;* imgtool_clearall — cdecl-safe wrapper around img_clearall.
;* The asm world is `flat,syscall` (no callee-saved regs);
;* MSVC cdecl expects EBX/ESI/EDI/EBP preserved across the call.
;* Without this wrapper, MSVC's optimizer can see corrupted
;* registers after the call → crash.
;*************************************************************
    public imgtool_clearall
imgtool_clearall proc near
    push    ebx
    push    esi
    push    edi
    push    ebp
    call    img_clearall
    pop     ebp
    pop     edi
    pop     esi
    pop     ebx
    ret
imgtool_clearall endp

;*************************************************************
;* imgtool_addnewpal — append a fresh blank 256-color palette
;*
;*   1. pal_alloc       (links new PAL into pal_p, inc palcnt, DATA_p=0)
;*   2. mem_alloc(768)  (256 colors × 3 bytes 6-bit RGB)
;*   3. zero the buffer, set NUMC=256, BITSPIX=8, FLAGS=0
;*
;* On any allocation failure we silently bail; partial state is
;* tolerable because pal_alloc already linked the empty PAL and
;* the user can delete it from the UI.
;*************************************************************
    public imgtool_addnewpal
imgtool_addnewpal proc near
    push    ebx
    push    ecx
    push    edi
    push    esi

    call    pal_alloc                   ; EAX = *PAL or 0
    test    eax, eax
    jz      done
    mov     esi, eax                    ; ESI = *PAL (preserve)

    ; Initialize header fields (PAL was zeroed only at NXT_p / DATA_p)
    mov     byte ptr [esi].PAL.FLAGS,   0
    mov     byte ptr [esi].PAL.BITSPIX, 8
    mov     word ptr [esi].PAL.NUMC,    256
    mov     word ptr [esi].PAL.pad,     0
    mov     byte ptr [esi].PAL.N_s,     0   ; empty name (null-terminated)

    ; Allocate 512-byte color data buffer
    mov     eax, 512
    call    mem_alloc                   ; EAX = *buf or 0
    test    eax, eax
    jz      done
    mov     [esi].PAL.DATA_p, eax

    ; Zero-fill the color buffer (256 × 2 bytes = 128 dwords)
    mov     edi, eax
    xor     eax, eax
    mov     ecx, 128
    rep     stosd

done:
    pop     esi
    pop     edi
    pop     ecx
    pop     ebx
    ret
imgtool_addnewpal endp

;*************************************************************
;* imgtool_img_alloc
;*************************************************************
    public imgtool_img_alloc
imgtool_img_alloc proc near
    push    ebx
    push    ecx
    push    edx
    push    esi
    push    edi
    push    ebp
    call    img_alloc
    pop     ebp
    pop     edi
    pop     esi
    pop     edx
    pop     ecx
    pop     ebx
    ret
imgtool_img_alloc endp

;*************************************************************
;* imgtool_pal_alloc
;*************************************************************
    public imgtool_pal_alloc
imgtool_pal_alloc proc near
    push    ebx
    push    ecx
    push    edx
    push    esi
    push    edi
    push    ebp
    call    pal_alloc
    pop     ebp
    pop     edi
    pop     esi
    pop     edx
    pop     ecx
    pop     ebx
    ret
imgtool_pal_alloc endp

;*************************************************************
;* imgtool_mem_alloc
;*************************************************************
    public imgtool_mem_alloc
imgtool_mem_alloc proc near
    push    ebx
    push    ecx
    push    edx
    push    esi
    push    edi
    push    ebp
    mov     eax, [esp + 28]  ; fetch size argument
    call    mem_alloc
    pop     ebp
    pop     edi
    pop     esi
    pop     edx
    pop     ecx
    pop     ebx
    ret
imgtool_mem_alloc endp

    end
