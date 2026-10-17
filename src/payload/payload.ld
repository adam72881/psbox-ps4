/* Un solo segmento, entry a vaddr 0. objcopy -O binary parte da _start. */
ENTRY(_start)
PHDRS { code PT_LOAD FLAGS(7); }
SECTIONS {
    . = 0;
    .text : {
        KEEP(*(.text.start))
        *(.text .text.*)
    } :code
    .rodata : ALIGN(16) { *(.rodata .rodata.*) } :code
    .data : ALIGN(16) { *(.data .data.*) } :code
    .bss : ALIGN(16) { *(.bss .bss.*) *(COMMON) } :code
}
