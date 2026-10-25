/*
 * GoldHEN Plugin SDK - a prx hook/patch sdk for Orbis OS
 * https://github.com/GoldHEN/GoldHEN_Plugins_SDK
 * MIT License. Crediti: SiSTRo, ctn123, OSM, jocover, bucanero, OpenOrbis, nik.
 *
 * Qui c'è solo l'interfaccia di jailbreak (syscall 500). Non ci sono
 * sys_dynlib_load_prx né la syscall 594.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct jailbreak_backup {
    uint32_t cr_uid;
    uint32_t cr_ruid;
    uint32_t cr_rgid;
    uint32_t cr_groups;
    uint64_t cr_paid;
    uint64_t cr_caps[2];
    void* cr_prison;
    void* fd_cdir;
    void* fd_jdir;
    void* fd_rdir;
};

#define GOLDHEN_SDK_CMD_VERSION 0
#define GOLDHEN_SDK_CMD_KLOG 1
#define GOLDHEN_SDK_CMD_JAILBREAK 2
#define GOLDHEN_SDK_CMD_UNJAILBREAK 3

int sys_sdk_cmd(uint64_t cmd, void* data);
int sys_sdk_jailbreak(struct jailbreak_backup* jb);
int sys_sdk_unjailbreak(struct jailbreak_backup* jb);

#ifdef __cplusplus
}
#endif
