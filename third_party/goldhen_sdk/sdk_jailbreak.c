/*
 * GoldHEN Plugin SDK - jailbreak di processo.
 * Stesso contratto di source/GoldHEN.c + source/Syscall.c del SDK ufficiale:
 * syscall indiretta (rax = 0) con numero 500 e comando JAILBREAK.
 * MIT License, GoldHEN/GoldHEN_Plugins_SDK.
 */
#include "GoldHEN.h"

#include <errno.h>
#include <stdint.h>

static int sdk_syscall(uint64_t num, uint64_t a1, uint64_t a2) {
    unsigned long rax;
    unsigned char cf;
    __asm__ volatile("syscall\n\t"
                     "setc %1"
                     : "=a"(rax), "=qm"(cf)
                     : "a"((unsigned long)0), "D"(num), "S"(a1), "d"(a2)
                     : "rcx", "r11", "memory");
    if (cf) {
        errno = (int)rax;
        return -1;
    }
    return (int)rax;
}

int sys_sdk_cmd(uint64_t cmd, void* data) {
    return sdk_syscall(500, cmd, (uint64_t)(uintptr_t)data);
}

int sys_sdk_jailbreak(struct jailbreak_backup* jb) {
    return sys_sdk_cmd(GOLDHEN_SDK_CMD_JAILBREAK, jb);
}

int sys_sdk_unjailbreak(struct jailbreak_backup* jb) {
    if (!jb) return -1;
    return sys_sdk_cmd(GOLDHEN_SDK_CMD_UNJAILBREAK, jb);
}
