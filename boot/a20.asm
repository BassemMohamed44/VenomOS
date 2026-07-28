; ============================================================
; a20.asm
;
; The A20 line: on the original 8086, addresses wrapped around
; at 1MB (address bit 20 was ignored). Later chips added a gate
; to keep the wraparound "on" for backward compatibility. Before
; we can safely address memory above 1MB (which Protected Mode
; and our future kernel need), we must switch that gate "off"
; so bit 20 of every address is honored.
;
; This uses the "Fast A20 Gate" via I/O port 0x92 - the System
; Control Port present on all PC-compatible chipsets (including
; QEMU's emulated chipset). It is simpler and more portable
; across real/virtual hardware than the older keyboard-controller
; method, which is why modern educational bootloaders prefer it.
; ============================================================

; enable_a20
; Sets bit 1 of I/O port 0x92 to enable the A20 line.
; Input : none
; Output: none (all registers preserved)
enable_a20:
    pusha
    in al, 0x92         ; read current system control port state
    test al, 2          ; is A20 already enabled (bit 1)?
    jnz .done
    or al, 2            ; set bit 1 (enable A20)
    and al, 0xFE        ; clear bit 0 (avoid triggering a fast reset)
    out 0x92, al
.done:
    popa
    ret
