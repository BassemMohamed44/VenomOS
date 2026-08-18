enable_a20:
    pusha
    in al, 0x92         
    test al, 2          
    jnz .done
    or al, 2            
    and al, 0xFE        
    out 0x92, al
.done:
    popa
    ret
