#ifndef ASB_NVIDIA_ADAPTER_HOOKS_H
#define ASB_NVIDIA_ADAPTER_HOOKS_H

#include <windows.h>
#include <stddef.h>

BOOL nvidia_install_adapter_hooks(void);
BOOL nvidia_get_icd_path(wchar_t *path, size_t capacity);
BOOL nvidia_map_luid_to_icd(LUID *luid);
BOOL nvidia_map_luid_to_guest(LUID *luid);

#endif
