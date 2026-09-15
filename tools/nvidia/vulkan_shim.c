#include "adapter_hooks.h"
#include <string.h>

typedef int VkResult;
typedef void *VkInstance;
typedef void *VkPhysicalDevice;
typedef void (WINAPI *VkFunction)(void);
typedef VkResult (WINAPI *NegotiateFn)(UINT *version);
typedef VkFunction (WINAPI *GetProcFn)(VkInstance instance, const char *name);
typedef VkResult (WINAPI *EnumerateFn)(VkInstance instance, LUID luid,
                                      UINT *count, VkPhysicalDevice *devices);
typedef void (WINAPI *PropertiesFn)(VkPhysicalDevice device, void *properties);

typedef struct VkProperty {
    int type;
    struct VkProperty *next;
} VkProperty;

typedef struct VkDeviceIdentity {
    int type;
    void *next;
    BYTE device_uuid[16];
    BYTE driver_uuid[16];
    BYTE luid[8];
    UINT node_mask;
    UINT luid_valid;
} VkDeviceIdentity;

#define VK_ERROR_INCOMPATIBLE_DRIVER (-9)
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES 1000071004
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES 50

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static INIT_ONCE g_interfaces_once = INIT_ONCE_STATIC_INIT;
static NegotiateFn g_negotiate;
static GetProcFn g_instance_proc, g_physical_proc;
static EnumerateFn g_enumerate;
static PVOID volatile g_properties, g_properties_khr;

VkResult WINAPI vk_icdEnumerateAdapterPhysicalDevices(VkInstance instance, LUID luid,
                                                      UINT *count, VkPhysicalDevice *devices);
VkFunction WINAPI vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char *name);

static BOOL CALLBACK load_vulkan_icd(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    wchar_t path[MAX_PATH];
    HMODULE driver;
    (void)once;
    (void)parameter;
    (void)context;

    if (!nvidia_get_icd_path(path, ARRAYSIZE(path))) return TRUE;
    driver = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!driver) return TRUE;

    g_negotiate = (NegotiateFn)GetProcAddress(driver, "vk_icdNegotiateLoaderICDInterfaceVersion");
    g_instance_proc = (GetProcFn)GetProcAddress(driver, "vk_icdGetInstanceProcAddr");
    g_physical_proc = (GetProcFn)GetProcAddress(driver, "vk_icdGetPhysicalDeviceProcAddr");
    g_enumerate = (EnumerateFn)GetProcAddress(driver, "vk_icdEnumerateAdapterPhysicalDevices");
    if (!g_negotiate && g_instance_proc)
        g_negotiate = (NegotiateFn)g_instance_proc(NULL, "vk_icdNegotiateLoaderICDInterfaceVersion");
    return TRUE;
}

static BOOL CALLBACK resolve_icd_interfaces(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    (void)once;
    (void)parameter;
    (void)context;

    if (g_instance_proc) {
        if (!g_physical_proc)
            g_physical_proc = (GetProcFn)g_instance_proc(NULL, "vk_icdGetPhysicalDeviceProcAddr");
        if (!g_enumerate)
            g_enumerate = (EnumerateFn)g_instance_proc(NULL, "vk_icdEnumerateAdapterPhysicalDevices");
    }
    return TRUE;
}

static void map_device_luids_to_guest(void *properties)
{
    VkProperty *property = ((VkProperty *)properties)->next;
    for (; property; property = property->next) {
        if (property->type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES ||
            property->type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES) {
            VkDeviceIdentity *identity = (VkDeviceIdentity *)property;
            LUID luid;
            if (!identity->luid_valid) continue;
            memcpy(&luid, identity->luid, sizeof(luid));
            if (nvidia_map_luid_to_guest(&luid))
                memcpy(identity->luid, &luid, sizeof(luid));
        }
    }
}

static void WINAPI get_physical_device_properties2_hook(VkPhysicalDevice device, void *properties)
{
    PropertiesFn function = (PropertiesFn)InterlockedCompareExchangePointer(&g_properties, NULL, NULL);
    function(device, properties);
    map_device_luids_to_guest(properties);
}

static void WINAPI get_physical_device_properties2_khr_hook(VkPhysicalDevice device, void *properties)
{
    PropertiesFn function = (PropertiesFn)InterlockedCompareExchangePointer(&g_properties_khr, NULL, NULL);
    function(device, properties);
    map_device_luids_to_guest(properties);
}

static VkFunction wrap_device_properties_proc(const char *name, VkFunction function)
{
    if (!function) return NULL;
    if (strcmp(name, "vkGetPhysicalDeviceProperties2") == 0) {
        InterlockedCompareExchangePointer(&g_properties, (PVOID)function, NULL);
        return (VkFunction)get_physical_device_properties2_hook;
    }
    if (strcmp(name, "vkGetPhysicalDeviceProperties2KHR") == 0) {
        InterlockedCompareExchangePointer(&g_properties_khr, (PVOID)function, NULL);
        return (VkFunction)get_physical_device_properties2_khr_hook;
    }
    return function;
}

VkResult WINAPI vk_icdNegotiateLoaderICDInterfaceVersion(UINT *version)
{
    VkResult result;
    InitOnceExecuteOnce(&g_once, load_vulkan_icd, NULL, NULL);
    if (!version || !g_negotiate || !g_instance_proc)
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    result = g_negotiate(version);
    if (result == 0)
        InitOnceExecuteOnce(&g_interfaces_once, resolve_icd_interfaces, NULL, NULL);
    return result;
}

VkFunction WINAPI vk_icdGetInstanceProcAddr(VkInstance instance, const char *name)
{
    InitOnceExecuteOnce(&g_once, load_vulkan_icd, NULL, NULL);
    if (!name || !g_instance_proc) return NULL;
    if (g_negotiate && strcmp(name, "vk_icdNegotiateLoaderICDInterfaceVersion") == 0)
        return (VkFunction)vk_icdNegotiateLoaderICDInterfaceVersion;
    InitOnceExecuteOnce(&g_interfaces_once, resolve_icd_interfaces, NULL, NULL);
    if (g_enumerate && strcmp(name, "vk_icdEnumerateAdapterPhysicalDevices") == 0)
        return (VkFunction)vk_icdEnumerateAdapterPhysicalDevices;
    if (g_physical_proc && strcmp(name, "vk_icdGetPhysicalDeviceProcAddr") == 0)
        return (VkFunction)vk_icdGetPhysicalDeviceProcAddr;
    return wrap_device_properties_proc(name, g_instance_proc(instance, name));
}

VkFunction WINAPI vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char *name)
{
    InitOnceExecuteOnce(&g_once, load_vulkan_icd, NULL, NULL);
    InitOnceExecuteOnce(&g_interfaces_once, resolve_icd_interfaces, NULL, NULL);
    return name && g_physical_proc ? wrap_device_properties_proc(name, g_physical_proc(instance, name)) : NULL;
}

VkResult WINAPI vk_icdEnumerateAdapterPhysicalDevices(VkInstance instance, LUID luid,
                                                      UINT *count, VkPhysicalDevice *devices)
{
    InitOnceExecuteOnce(&g_once, load_vulkan_icd, NULL, NULL);
    InitOnceExecuteOnce(&g_interfaces_once, resolve_icd_interfaces, NULL, NULL);
    if (!count || !g_enumerate) return VK_ERROR_INCOMPATIBLE_DRIVER;
    nvidia_map_luid_to_icd(&luid);
    return g_enumerate(instance, luid, count, devices);
}

DWORD WINAPI appsandbox_nvidia(void)
{
    return 0x41534201;
}
