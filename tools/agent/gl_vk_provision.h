#ifndef GL_VK_PROVISION_H
#define GL_VK_PROVISION_H

#include <windows.h>

BOOL gpu_prefers_system_opengl(void);
BOOL gl_vk_provision_runtime(const wchar_t *dir, const wchar_t *native_dir,
                             const wchar_t *sys, BOOL *native_runtime);
BOOL nvidia_dlss_provision(const wchar_t *native_dir);

#endif
