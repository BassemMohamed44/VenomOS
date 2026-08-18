gdt_start:

gdt_null:
    dq 0x0

gdt_code32:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10011010b
    db 11001111b
    db 0x00

gdt_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b
    db 11001111b
    db 0x00

gdt_code64:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10011010b
    db 10101111b
    db 0x00

gdt_user_code:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 11111010b
    db 10101111b
    db 0x00

gdt_user_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 11110010b
    db 11001111b
    db 0x00

tss_start:
    dd 0            
    dq 0            
    dq 0            
    dq 0            
    dq 0            
    times 7 dq 0     
    times 5 dw 0      
    dw tss_end - tss_start 
tss_end:

gdt_tss:
    dw (tss_end - tss_start) - 1 
    dw 0x0000                     
    db 0x00                        
    db 10001001b                    
    db 0x00                          
    db 0x00                           
    dd 0x00000000                      
    dd 0                                

gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1 
    dd gdt_start                

CODE32_SEG    equ gdt_code32    - gdt_start
DATA_SEG      equ gdt_data      - gdt_start
CODE64_SEG    equ gdt_code64    - gdt_start
USER_CODE_SEG equ gdt_user_code - gdt_start
USER_DATA_SEG equ gdt_user_data - gdt_start
TSS_SEG       equ gdt_tss       - gdt_start

USER_CODE_SEL equ USER_CODE_SEG | 3
USER_DATA_SEL equ USER_DATA_SEG | 3
