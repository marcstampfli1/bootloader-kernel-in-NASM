org 0
bits 16

start:
  mov ax, cs
  mov ds, ax
  mov es, ax
  
  cli
  mov ax, cs
  mov ss, ax
  mov sp, 0xFFFC
  sti

  cld
  mov si, hello_world
  mov ah, 0x0E
  mov bh, 0
  mov bl, 0x07

print_char:
  lodsb
  test al, al
  jz done
  int 0x10
  jmp print_char

done:
  hlt
  jmp done

hello_world db "Hello World!", 0

times 512 - ($ - $$) % 512 db 0
