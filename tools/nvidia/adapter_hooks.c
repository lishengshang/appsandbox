#include "adapter_hooks.h"
#include <stdio.h>
#include <string.h>

#if defined(_M_X64) || defined(_M_IX86)
#include <winternl.h>
#include <d3dkmthk.h>

#if defined(_M_IX86)
#define NVIDIA_ICD_NAME "nvoglv32.dll"
#define NVIDIA_ICD_NAME_W L"nvoglv32.dll"
#else
#define NVIDIA_ICD_NAME "nvoglv64.dll"
#define NVIDIA_ICD_NAME_W L"nvoglv64.dll"
#endif

typedef NTSTATUS (NTAPI *EnumDisplayDevicesFn)(PUNICODE_STRING, DWORD,
                                             PDISPLAY_DEVICEW, DWORD);
typedef int (WINAPI *CudaInitFn)(unsigned);
typedef int (WINAPI *CudaDeviceCountFn)(int *);
typedef int (WINAPI *CudaDeviceGetFn)(int *, int);
typedef int (WINAPI *CudaDevicePciFn)(char *, int, int);
typedef int (WINAPI *CudaDeviceLuidFn)(char *, unsigned *, int);

typedef struct {
    LUID luid;
    D3DKMT_DEVICE_IDS ids;
    D3DKMT_ADAPTERADDRESS address;
    BOOL has_address;
} NvidiaAdapter;

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static BOOL g_ready;
static volatile LONG g_hooks_enabled;
static PFND3DKMT_ENUMADAPTERS2 g_enum_adapters;
static PFND3DKMT_QUERYADAPTERINFO g_query_adapter;
static PFND3DKMT_OPENADAPTERFROMLUID g_open_luid;
static PFND3DKMT_CLOSEADAPTER g_close_adapter;
static PFND3DKMT_OPENADAPTERFROMHDC g_open_hdc;
static EnumDisplayDevicesFn g_enum_displays;
static NvidiaAdapter *g_adapters;
static ULONG g_adapter_count;
static LUID g_luid;
static LUID g_icd_luid;
static wchar_t g_icd_path[MAX_PATH];
static DISPLAY_DEVICEW g_display;

static BOOL same_luid(LUID a, LUID b)
{
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

static BOOL query_adapter(D3DKMT_HANDLE adapter, KMTQUERYADAPTERINFOTYPE type,
                         void *data, UINT size)
{
    D3DKMT_QUERYADAPTERINFO query = {0};
    query.hAdapter = adapter;
    query.Type = type;
    query.pPrivateDriverData = data;
    query.PrivateDriverDataSize = size;
    return g_query_adapter(&query) >= 0;
}

static BOOL resolve_adapter_icd_path(D3DKMT_HANDLE adapter, wchar_t *path, size_t capacity)
{
    D3DKMT_OPENGLINFO info = {0};
    const wchar_t *name, *source;
    wchar_t directory[MAX_PATH];
    DWORD attributes;

    if (!query_adapter(adapter, KMTQAITYPE_UMOPENGLINFO, &info, sizeof(info)))
        return FALSE;
    info.UmdOpenGlIcdFileName[MAX_PATH - 1] = L'\0';
    source = info.UmdOpenGlIcdFileName;
    name = wcsrchr(source, L'\\');
    name = name ? name + 1 : source;
    if (_wcsicmp(name, NVIDIA_ICD_NAME_W) != 0) return FALSE;

    if (wcsncmp(source, L"\\??\\", 4) == 0) source += 4;
    if (_wcsnicmp(source, L"\\SystemRoot\\", 12) == 0) {
        if (!GetWindowsDirectoryW(directory, MAX_PATH)) return FALSE;
        if (swprintf_s(path, capacity, L"%s\\%s", directory, source + 12) < 0)
            return FALSE;
    } else if (source[0] && source[1] == L':' && source[2] == L'\\') {
        if (wcslen(source) >= capacity) return FALSE;
        wcscpy_s(path, capacity, source);
    } else if (source == name) {
        if (!GetSystemDirectoryW(directory, MAX_PATH)) return FALSE;
        if (swprintf_s(path, capacity, L"%s\\%s", directory, source) < 0)
            return FALSE;
    } else {
        return FALSE;
    }
#if defined(_M_IX86)
    if (GetWindowsDirectoryW(directory, MAX_PATH)) {
        size_t length = wcslen(directory);
        if (_wcsnicmp(path, directory, length) == 0 &&
            _wcsnicmp(path + length, L"\\System32\\", 10) == 0 &&
            wcslen(path) + 1 < capacity) {
            wchar_t native[MAX_PATH];
            if (swprintf_s(native, ARRAYSIZE(native), L"%s\\Sysnative%s",
                           directory, path + length + 9) < 0)
                return FALSE;
            wcscpy_s(path, capacity, native);
        }
    }
#endif
    attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const wchar_t *store = path;
        wchar_t translated[MAX_PATH];
        while (*store && _wcsnicmp(store, L"\\DriverStore\\", 13) != 0) ++store;
        if (*store && wcslen(path) + 4 < capacity &&
            wcslen(path) + 4 < ARRAYSIZE(translated)) {
            if (swprintf_s(translated, ARRAYSIZE(translated), L"%.*s\\HostDriverStore%s",
                           (int)(store - path), path, store + 12) < 0)
                return FALSE;
            attributes = GetFileAttributesW(translated);
            if (attributes != INVALID_FILE_ATTRIBUTES)
                wcscpy_s(path, capacity, translated);
        }
    }
    return attributes != INVALID_FILE_ATTRIBUTES &&
           !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL find_cuda_luid(const D3DKMT_ADAPTERADDRESS *address, LUID *luid)
{
    HMODULE cuda = LoadLibraryExW(L"nvcuda.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    CudaInitFn init;
    CudaDeviceCountFn get_count;
    CudaDeviceGetFn get_device;
    CudaDevicePciFn get_pci;
    CudaDeviceLuidFn get_luid;
    LUID selected = {0};
    int count, matches = 0;

    if (!cuda) return FALSE;
    init = (CudaInitFn)GetProcAddress(cuda, "cuInit");
    get_count = (CudaDeviceCountFn)GetProcAddress(cuda, "cuDeviceGetCount");
    get_device = (CudaDeviceGetFn)GetProcAddress(cuda, "cuDeviceGet");
    get_pci = (CudaDevicePciFn)GetProcAddress(cuda, "cuDeviceGetPCIBusId");
    get_luid = (CudaDeviceLuidFn)GetProcAddress(cuda, "cuDeviceGetLuid");
    if (!init || !get_count || !get_device || !get_pci || !get_luid ||
        init(0) != 0 || get_count(&count) != 0 || count <= 0)
        goto failed;

    for (int i = 0; i < count; ++i) {
        char pci[64] = {0}, trailing;
        unsigned domain, bus, device, function, node_mask = 0;
        int cuda_device;

        if (get_device(&cuda_device, i) != 0 ||
            get_pci(pci, (int)sizeof(pci), cuda_device) != 0)
            goto failed;
        pci[sizeof(pci) - 1] = '\0';
        if (sscanf_s(pci, "%x:%x:%x.%x%c", &domain, &bus, &device, &function,
                     &trailing, 1u) != 4)
            goto failed;
        if (bus != address->BusNumber || device != address->DeviceNumber ||
            function != address->FunctionNumber)
            continue;
        if (++matches != 1 || get_luid((char *)&selected, &node_mask, cuda_device) != 0 ||
            node_mask != 1 || (!selected.LowPart && !selected.HighPart))
            goto failed;
    }
    if (matches != 1) goto failed;
    *luid = selected;
    return TRUE;

failed:
    FreeLibrary(cuda);
    return FALSE;
}

static BOOL select_nvidia_adapter(void)
{
    D3DKMT_ENUMADAPTERS2 enumeration = {0};
    D3DKMT_ADAPTERINFO *adapters;
    NvidiaAdapter selected = {0};
    ULONG capacity, best_sources = 0;
    BOOL found = FALSE;

    if (g_enum_adapters(&enumeration) < 0 || !enumeration.NumAdapters)
        return FALSE;
    capacity = enumeration.NumAdapters;
    adapters = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                         capacity * sizeof(*adapters));
    g_adapters = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                           capacity * sizeof(*g_adapters));
    if (!adapters || !g_adapters) {
        if (adapters) HeapFree(GetProcessHeap(), 0, adapters);
        return FALSE;
    }
    enumeration.pAdapters = adapters;
    if (g_enum_adapters(&enumeration) >= 0 && enumeration.NumAdapters <= capacity) {
        for (ULONG i = 0; i < enumeration.NumAdapters; ++i) {
            D3DKMT_ADAPTERTYPE type = {0};
            D3DKMT_QUERY_DEVICE_IDS ids = {0};
            wchar_t path[MAX_PATH];

            if (!query_adapter(adapters[i].hAdapter, KMTQAITYPE_ADAPTERTYPE_RENDER,
                               &type, sizeof(type)) || !type.Paravirtualized ||
                !query_adapter(adapters[i].hAdapter, KMTQAITYPE_PHYSICALADAPTERDEVICEIDS,
                               &ids, sizeof(ids)) || ids.DeviceIds.VendorID != 0x10de)
                continue;
            g_adapters[g_adapter_count].luid = adapters[i].AdapterLuid;
            g_adapters[g_adapter_count].ids = ids.DeviceIds;
            g_adapters[g_adapter_count].has_address = query_adapter(adapters[i].hAdapter,
                KMTQAITYPE_ADAPTERADDRESS_RENDER, &g_adapters[g_adapter_count].address,
                sizeof(g_adapters[g_adapter_count].address));
            ++g_adapter_count;
            if ((found && adapters[i].NumOfSources <= best_sources) ||
                !resolve_adapter_icd_path(adapters[i].hAdapter, path, MAX_PATH))
                continue;
            found = TRUE;
            best_sources = adapters[i].NumOfSources;
            g_luid = adapters[i].AdapterLuid;
            selected = g_adapters[g_adapter_count - 1];
            wcscpy_s(g_icd_path, MAX_PATH, path);
        }
    }
    for (ULONG i = 0; i < capacity; ++i) {
        if (adapters[i].hAdapter) {
            D3DKMT_CLOSEADAPTER close = {adapters[i].hAdapter};
            g_close_adapter(&close);
        }
    }
    HeapFree(GetProcessHeap(), 0, adapters);
    if (!found) return FALSE;

    capacity = g_adapter_count;
    g_adapter_count = 0;
    for (ULONG i = 0; i < capacity; ++i) {
        if (same_luid(g_adapters[i].luid, selected.luid) ||
            (selected.has_address && g_adapters[i].has_address &&
             memcmp(&g_adapters[i].ids, &selected.ids, sizeof(selected.ids)) == 0 &&
             memcmp(&g_adapters[i].address, &selected.address, sizeof(selected.address)) == 0))
            g_adapters[g_adapter_count++] = g_adapters[i];
    }
    g_icd_luid = g_luid;
    if (selected.has_address) find_cuda_luid(&selected.address, &g_icd_luid);
    swprintf_s(g_display.DeviceID, ARRAYSIZE(g_display.DeviceID),
               L"PCI\\VEN_%04X&DEV_%04X&SUBSYS_%04X%04X&REV_%02X",
               selected.ids.VendorID, selected.ids.DeviceID, selected.ids.SubSystemID,
               selected.ids.SubVendorID, selected.ids.RevisionID);
    wcscpy_s(g_display.DeviceString, ARRAYSIZE(g_display.DeviceString), L"NVIDIA");
    g_display.cb = sizeof(g_display);
    g_display.StateFlags = DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE;
    return TRUE;
}

static BOOL find_desktop_display_name(void)
{
    DISPLAY_DEVICEW display = {0};

    for (DWORD i = 0; ; ++i) {
        display.cb = sizeof(display);
        if (g_enum_displays(NULL, i, &display, 0) < 0) break;
        if (display.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) {
            wcscpy_s(g_display.DeviceName, ARRAYSIZE(g_display.DeviceName), display.DeviceName);
            return TRUE;
        }
        if (!g_display.DeviceName[0] &&
            (display.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))
            wcscpy_s(g_display.DeviceName, ARRAYSIZE(g_display.DeviceName), display.DeviceName);
    }
    return g_display.DeviceName[0] != L'\0';
}

static BOOL call_stack_contains_nvidia_icd(void)
{
    void *frames[24];
    USHORT count = CaptureStackBackTrace(1, ARRAYSIZE(frames), frames, NULL);

    /* Discovery runs inside the ICD's DllMain, so do not acquire the loader lock. */
    for (USHORT i = 0; i < count; ++i) {
        MEMORY_BASIC_INFORMATION memory;
        if (!VirtualQuery(frames[i], &memory, sizeof(memory)) || memory.Type != MEM_IMAGE)
            continue;
        __try {
            BYTE *base = memory.AllocationBase;
            IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
            IMAGE_NT_HEADERS *nt;
            IMAGE_DATA_DIRECTORY directory;
            IMAGE_EXPORT_DIRECTORY *exports;
            DWORD size;

            if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) continue;
            nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) continue;
            size = nt->OptionalHeader.SizeOfImage;
            directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            if (!directory.VirtualAddress || size < sizeof(*exports) ||
                directory.VirtualAddress > size - sizeof(*exports))
                continue;
            exports = (IMAGE_EXPORT_DIRECTORY *)(base + directory.VirtualAddress);
            if (exports->Name && exports->Name <= size - sizeof(NVIDIA_ICD_NAME) &&
                _stricmp((const char *)(base + exports->Name), NVIDIA_ICD_NAME) == 0)
                return TRUE;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return FALSE;
}

static NTSTATUS NTAPI enum_display_devices_hook(PUNICODE_STRING device, DWORD index,
                                   PDISPLAY_DEVICEW display, DWORD flags)
{
    if (InterlockedCompareExchange(&g_hooks_enabled, 0, 0) &&
        (!device || !device->Buffer || !device->Length) && display &&
        display->cb >= sizeof(*display) && call_stack_contains_nvidia_icd()) {
        if (index == 0) {
            DWORD size = display->cb;
            *display = g_display;
            display->cb = size;
            return 0;
        }
        --index;
    }
    return g_enum_displays(device, index, display, flags);
}

static NTSTATUS APIENTRY open_adapter_from_hdc_hook(D3DKMT_OPENADAPTERFROMHDC *adapter)
{
    NTSTATUS status = g_open_hdc(adapter);

    if (status >= 0 && adapter && InterlockedCompareExchange(&g_hooks_enabled, 0, 0) &&
        call_stack_contains_nvidia_icd()) {
        if (!same_luid(adapter->AdapterLuid, g_luid)) {
            D3DKMT_OPENADAPTERFROMLUID replacement = {0};
            replacement.AdapterLuid = g_luid;
            if (g_open_luid(&replacement) >= 0) {
                D3DKMT_CLOSEADAPTER close = {adapter->hAdapter};
                if (g_close_adapter(&close) >= 0) {
                    adapter->hAdapter = replacement.hAdapter;
                    adapter->AdapterLuid = replacement.AdapterLuid;
                    adapter->VidPnSourceId = 0;
                } else {
                    close.hAdapter = replacement.hAdapter;
                    g_close_adapter(&close);
                }
            }
        }
        if (same_luid(adapter->AdapterLuid, g_luid)) adapter->AdapterLuid = g_icd_luid;
    }
    return status;
}

static void *volatile *find_win32u_import_slot(HMODULE module, const char *name)
{
    BYTE *base = (BYTE *)module;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt;
    IMAGE_IMPORT_DESCRIPTOR *imports;
    DWORD rva;

    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        return NULL;
    nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;
    rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return NULL;
    imports = (IMAGE_IMPORT_DESCRIPTOR *)(base + rva);
    for (; imports->Name; ++imports) {
        IMAGE_THUNK_DATA *names, *addresses;
        if (_stricmp((const char *)(base + imports->Name), "win32u.dll") != 0 ||
            !imports->OriginalFirstThunk || !imports->FirstThunk)
            continue;
        names = (IMAGE_THUNK_DATA *)(base + imports->OriginalFirstThunk);
        addresses = (IMAGE_THUNK_DATA *)(base + imports->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addresses) {
            IMAGE_IMPORT_BY_NAME *entry;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            entry = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((const char *)entry->Name, name) == 0)
                return (void *volatile *)&addresses->u1.Function;
        }
    }
    return NULL;
}

static BOOL install_win32u_import_hooks(HMODULE user32, HMODULE gdi32)
{
    void *volatile *display_slot = find_win32u_import_slot(user32, "NtUserEnumDisplayDevices");
    void *volatile *adapter_slot = find_win32u_import_slot(gdi32, "NtGdiDdDDIOpenAdapterFromHdc");
    void *display_original, *adapter_original;
    DWORD display_protection, adapter_protection, unused;
    HMODULE pinned;
    BOOL installed = FALSE;

    if (!display_slot || !adapter_slot || !*display_slot || !*adapter_slot)
        return FALSE;
    display_original = *display_slot;
    adapter_original = *adapter_slot;
    if (display_original == (void *)enum_display_devices_hook || adapter_original == (void *)open_adapter_from_hdc_hook)
        return FALSE;
    g_enum_displays = (EnumDisplayDevicesFn)display_original;
    g_open_hdc = (PFND3DKMT_OPENADAPTERFROMHDC)adapter_original;

    if (!VirtualProtect((void *)display_slot, sizeof(*display_slot), PAGE_READWRITE,
                        &display_protection))
        return FALSE;
    if (!VirtualProtect((void *)adapter_slot, sizeof(*adapter_slot), PAGE_READWRITE,
                        &adapter_protection)) {
        VirtualProtect((void *)display_slot, sizeof(*display_slot), display_protection, &unused);
        return FALSE;
    }
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                          (LPCWSTR)nvidia_install_adapter_hooks, &pinned) &&
        InterlockedCompareExchangePointer(display_slot, (void *)enum_display_devices_hook,
                                          display_original) == display_original) {
        if (InterlockedCompareExchangePointer(adapter_slot, (void *)open_adapter_from_hdc_hook,
                                              adapter_original) == adapter_original) {
            InterlockedExchange(&g_hooks_enabled, TRUE);
            installed = TRUE;
        } else {
            InterlockedCompareExchangePointer(display_slot, display_original, (void *)enum_display_devices_hook);
        }
    }
    VirtualProtect((void *)adapter_slot, sizeof(*adapter_slot), adapter_protection, &unused);
    VirtualProtect((void *)display_slot, sizeof(*display_slot), display_protection, &unused);
    return installed;
}

static BOOL CALLBACK initialize_adapter_hooks(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    HMODULE user32, gdi32;
    (void)once;
    (void)parameter;
    (void)context;

    user32 = LoadLibraryExW(L"user32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    gdi32 = LoadLibraryExW(L"gdi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!user32 || !gdi32) goto done;
    if (GetModuleHandleW(NVIDIA_ICD_NAME_W)) goto done;
    g_enum_adapters = (PFND3DKMT_ENUMADAPTERS2)GetProcAddress(gdi32, "D3DKMTEnumAdapters2");
    g_query_adapter = (PFND3DKMT_QUERYADAPTERINFO)GetProcAddress(gdi32, "D3DKMTQueryAdapterInfo");
    g_open_luid = (PFND3DKMT_OPENADAPTERFROMLUID)GetProcAddress(gdi32, "D3DKMTOpenAdapterFromLuid");
    g_close_adapter = (PFND3DKMT_CLOSEADAPTER)GetProcAddress(gdi32, "D3DKMTCloseAdapter");
    if (g_enum_adapters && g_query_adapter && g_open_luid && g_close_adapter) {
        void *volatile *slot = find_win32u_import_slot(user32, "NtUserEnumDisplayDevices");
        if (slot) g_enum_displays = (EnumDisplayDevicesFn)*slot;
        if (g_enum_displays && select_nvidia_adapter() && !GetModuleHandleW(NVIDIA_ICD_NAME_W) &&
            find_desktop_display_name())
            g_ready = install_win32u_import_hooks(user32, gdi32);
    }
done:
    if (!g_ready) {
        if (g_adapters) HeapFree(GetProcessHeap(), 0, g_adapters);
        g_adapters = NULL;
        g_adapter_count = 0;
        if (user32) FreeLibrary(user32);
        if (gdi32) FreeLibrary(gdi32);
    }
    return TRUE;
}

BOOL nvidia_install_adapter_hooks(void)
{
    InitOnceExecuteOnce(&g_once, initialize_adapter_hooks, NULL, NULL);
    return g_ready;
}

BOOL WINAPI appsandbox_nvidia_adapter_luid(LUID *luid)
{
    if (!luid || !InterlockedCompareExchange(&g_hooks_enabled, 0, 0)) return FALSE;
    *luid = g_luid;
    return TRUE;
}

BOOL nvidia_get_icd_path(wchar_t *path, size_t capacity)
{
    if (!path || !capacity) return FALSE;
    path[0] = L'\0';
    if (!nvidia_install_adapter_hooks() || wcslen(g_icd_path) >= capacity) return FALSE;
    wcscpy_s(path, capacity, g_icd_path);
    return TRUE;
}

BOOL nvidia_map_luid_to_icd(LUID *luid)
{
    if (!luid || !nvidia_install_adapter_hooks()) return FALSE;
    for (ULONG i = 0; i < g_adapter_count; ++i) {
        if (same_luid(*luid, g_adapters[i].luid)) {
            *luid = g_icd_luid;
            return TRUE;
        }
    }
    return FALSE;
}

BOOL nvidia_map_luid_to_guest(LUID *luid)
{
    if (!luid || !nvidia_install_adapter_hooks() || !same_luid(*luid, g_icd_luid)) return FALSE;
    *luid = g_luid;
    return TRUE;
}

#else

BOOL nvidia_install_adapter_hooks(void)
{
    return FALSE;
}

BOOL WINAPI appsandbox_nvidia_adapter_luid(LUID *luid)
{
    (void)luid;
    return FALSE;
}

BOOL nvidia_get_icd_path(wchar_t *path, size_t capacity)
{
    if (path && capacity) path[0] = L'\0';
    return FALSE;
}

BOOL nvidia_map_luid_to_icd(LUID *luid)
{
    (void)luid;
    return FALSE;
}

BOOL nvidia_map_luid_to_guest(LUID *luid)
{
    (void)luid;
    return FALSE;
}

#endif
