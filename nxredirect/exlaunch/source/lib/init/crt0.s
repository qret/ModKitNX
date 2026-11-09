.section ".text.crt0","ax"

.macro FROM_MOD0 register_num, offset
    ldr w\register_num, [x24, #\offset]
    sxtw x\register_num, w\register_num
    add x\register_num, x\register_num, x24
.endm

.macro FUNC_RELATIVE_ASLR name, register_num, symbol
.word \symbol - .
\name:
    ldr w\register_num, [x30]
    sxtw x\register_num, w\register_num
    add x\register_num, x\register_num, x30
.endm

.global __module_start
__module_start:
    b entrypoint
    .word __nx_mod0 - __module_start

    // ВАЖНО (проверено на консоли): rtld этой игры читает слово по смещению +8
    // как смещение до структуры module-runtime-info и копирует оттуда 12 байт.
    // В шаблоне здесь стояло выравнивание (.align 4 -> два NOP 0xd503201f), rtld брал
    // NOP за смещение, уходил по мусорному адресу и ронял процесс (Data Abort, fault 0).
    // Кладём сюда честное смещение на нулевую структуру: версия 0 < 0x12, поэтому
    // строгие проверки rtld пропускает. Раскладка не меняется: строка ниже занимает
    // прежние 16 байт, entrypoint остаётся на +0x20.
    .word __nx_module_runtime_info - __module_start
    .word 0
    .ascii "~~exlaunch uwu~~"

entrypoint:
   // Arguments on NSO entry:
    //   x0=zero                  | x1=main thread handle
    // Arguments on NRO entry (homebrew ABI):
    //   x0=ptr to env context    | x1=UINT64_MAX (-1 aka 0xFFFFFFFFFFFFFFFF)
    // Arguments on user-mode exception entry:
    //   x0=excpt type (non-zero) | x1=ptr to excpt context

    // Detect and handle user-mode exceptions first:
    // if (x0 != 0 && x1 != UINT64_MAX) exl_exception_entry(<inargs>);
    cmp  x0, #0
    ccmn x1, #1, #4, ne // 4 = Z
    beq  get_module_info
    b exl_exception_entry

get_module_info:
    // Get start of our module
    bl __get_module_start_shim
    FUNC_RELATIVE_ASLR __get_module_start_shim, 23, __module_start
    
    // Get location of MOD0
    mov x24, xzr
    ldr w24, [x23, #4]
    add x24, x24, x23

    // Get BSS regions from MOD0
    FROM_MOD0 8, 0x8
    FROM_MOD0 9, 0xC

bssclr_start:
    // Calculate BSS address/size
    sub  x9, x9, x8        // calculate BSS size
    add  x9, x9, #7        // round up to 8
    bic  x9, x9, #7        // ^

    // Clear the BSS in 8-byte units
bss_loop:
    subs w9, w9, #8
    str  xzr, [x8], #8
    bne  bss_loop

    // Preserve registers across function calls
    mov x25, x0  // entrypoint argument 0
    mov x26, x1  // entrypoint argument 1

    // Parse ELF .dynamic section (which applies relocations to our module)
    mov x0, x23
    FROM_MOD0 1, 0x4
    bl   exl_relocate_self

    mov  x0, x25
    mov  x1, x26
    b exl_entrypoint_init

    // failsafe
    .word 0xdeadbeef


.section ".rodata.mod0","a"

.hidden exl_nx_module_runtime 

.align 2
// Структура module-runtime-info, на которую ссылается слово по смещению +8 от начала
// модуля (см. комментарий выше). Нули = версия 0: rtld копирует эти 12 байт и на этом
// успокаивается. У штатных модулей игры здесь {0x15, 4, 4}.
.global __nx_module_runtime_info
.hidden __nx_module_runtime_info
__nx_module_runtime_info:
    .word 0
    .word 0
    .word 0

__nx_mod0:
    .ascii "MOD0"
    .word  __dynamic_start__        - __nx_mod0
    .word  __bss_start__            - __nx_mod0
    .word  __bss_end__              - __nx_mod0
    .word  __eh_frame_hdr_start__   - __nx_mod0
    .word  __eh_frame_hdr_end__     - __nx_mod0
    .word  exl_nx_module_runtime    - __nx_mod0