E820_BUFFER_ADDR equ 0x9000
E820_COUNT_ADDR  equ 0x8FF8    
E820_MAX_ENTRIES equ 64        

detect_memory_map:
    pusha
    push es

    xor ax, ax
    mov es, ax
    mov di, E820_BUFFER_ADDR

    xor ebx, ebx         
    xor bp, bp            

.loop:
    mov eax, 0xE820
    mov edx, 0x534D4150   
    mov ecx, 24            
    int 0x15

    jc .done               

    cmp eax, 0x534D4150     
    jne .done

    inc bp
    add di, 24

    cmp bp, E820_MAX_ENTRIES
    jae .done               

    test ebx, ebx
    jnz .loop                

.done:
    mov [E820_COUNT_ADDR], bp
    pop es
    popa
    ret
