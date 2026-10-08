// Layover: drop-in replacement for AMD's AGS 6.1 runtime (amd_ags_x64.dll).
//
// Games that see an AMD adapter (D3DMetal reports one) initialise AGS and treat a failure as
// "no graphics driver installed". The real AGS needs AMD's ADL driver library, which does not
// exist under Wine. This build answers with a plausible single-GPU description taken from DXGI
// and creates D3D12 devices through plain D3D12CreateDevice, with no vendor extensions.
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define AMD_AGS_API __declspec(dllexport)
#include "amd_ags.h"
#undef AMD_AGS_API
#define AMD_AGS_API extern "C" __declspec(dllexport)
// 6.1 header lacks these two (added in 6.2); declare them so they export unmangled too.
AMD_AGS_API AGSReturnCode agsDriverExtensionsDX12_CreateFromDevice(AGSContext *, ID3D12Device *, AGSDX12ReturnedParams *);
AMD_AGS_API AGSReturnCode agsDriverExtensionsDX12_Destroy(AGSContext *, ID3D12Device *, unsigned int *);

struct AGSContext {
    AGSConfiguration cfg;
    AGSDeviceInfo device;
    AGSDisplayInfo display;
    char adapterName[128];
};

static void *ags_alloc(const AGSConfiguration *c, size_t n) {
    void *p = (c && c->allocCallback) ? c->allocCallback(n) : malloc(n);
    if (p) memset(p, 0, n);
    return p;
}
static void ags_free(const AGSConfiguration *c, void *p) {
    if (!p) return;
    if (c && c->freeCallback) c->freeCallback(p); else free(p);
}

static void fill_display(AGSDisplayInfo *d) {
    strcpy(d->name, "Display");
    strcpy(d->displayDeviceName, "\\\\.\\DISPLAY1");
    d->isPrimaryDisplay = 1;
    int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
    DISPLAY_DEVICEA dd = {}; dd.cb = sizeof dd;
    if (EnumDisplayDevicesA(NULL, 0, &dd, 0)) {
        strncpy(d->displayDeviceName, dd.DeviceName, sizeof d->displayDeviceName - 1);
        strncpy(d->name, dd.DeviceString, sizeof d->name - 1);
    }
    DEVMODEA dm = {}; dm.dmSize = sizeof dm;
    float hz = 60.0f;
    if (EnumDisplaySettingsA(dd.DeviceName[0] ? dd.DeviceName : NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        hz = (float)dm.dmDisplayFrequency;
    d->maxResolutionX = w; d->maxResolutionY = h; d->maxRefreshRate = hz;
    d->currentResolution.offsetX = 0; d->currentResolution.offsetY = 0; d->currentResolution.width = w; d->currentResolution.height = h;
    d->visibleResolution = d->currentResolution;
    d->currentRefreshRate = hz;
    // sRGB / D65 primaries, SDR luminance
    d->chromaticityRedX = 0.64; d->chromaticityRedY = 0.33; d->chromaticityGreenX = 0.30; d->chromaticityGreenY = 0.60;
    d->chromaticityBlueX = 0.15; d->chromaticityBlueY = 0.06; d->chromaticityWhitePointX = 0.3127; d->chromaticityWhitePointY = 0.3290;
    d->minLuminance = 0.1; d->maxLuminance = 400.0; d->avgLuminance = 200.0;
    d->logicalDisplayIndex = 0; d->adlAdapterIndex = 0;
}

static bool fill_device(AGSContext *ctx) {
    AGSDeviceInfo *dev = &ctx->device;
    strcpy(ctx->adapterName, "AMD Radeon Graphics");
    dev->vendorId = 0x1002; dev->deviceId = 0x73bf; dev->revisionId = 0xc1;
    dev->localMemoryInBytes = 8ull << 30; dev->sharedMemoryInBytes = 16ull << 30;
    IDXGIFactory1 *f = NULL;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) {
        IDXGIAdapter1 *a = NULL;
        if (f->EnumAdapters1(0, &a) == S_OK) {
            DXGI_ADAPTER_DESC1 desc;
            if (SUCCEEDED(a->GetDesc1(&desc))) {
                WideCharToMultiByte(CP_ACP, 0, desc.Description, -1, ctx->adapterName, sizeof ctx->adapterName - 1, NULL, NULL);
                dev->vendorId = desc.VendorId; dev->deviceId = desc.DeviceId; dev->revisionId = desc.Revision;
                if (desc.DedicatedVideoMemory) dev->localMemoryInBytes = desc.DedicatedVideoMemory;
                if (desc.SharedSystemMemory) dev->sharedMemoryInBytes = desc.SharedSystemMemory;
            }
            a->Release();
        }
        f->Release();
    }
    dev->adapterString = ctx->adapterName;
    dev->asicFamily = (dev->vendorId == 0x1002) ? AGSDeviceInfo::AsicFamily_RDNA2 : AGSDeviceInfo::AsicFamily_Unknown;
    dev->isAPU = 0; dev->isPrimaryDevice = 1; dev->isExternal = 0;
    dev->numCUs = 60; dev->numWGPs = 30; dev->numROPs = 96;
    dev->coreClock = 2105; dev->memoryClock = 2000; dev->memoryBandwidth = 512000;
    dev->teraFlops = 16.2f;
    dev->numDisplays = 1; dev->displays = &ctx->display;
    dev->adlAdapterIndex = 0;
    fill_display(&ctx->display);
    return true;
}

AMD_AGS_API int agsGetVersionNumber() { return AGS_CURRENT_VERSION; }

AMD_AGS_API AGSDriverVersionResult agsCheckDriverVersion(const char *, unsigned int) { return AGS_SOFTWAREVERSIONCHECK_OK; }

AMD_AGS_API AGSReturnCode agsInitialize(int agsVersion, const AGSConfiguration *config, AGSContext **context, AGSGPUInfo *gpuInfo) {
    (void)agsVersion;   // accept any 6.x caller
    if (!context) return AGS_INVALID_ARGS;
    AGSContext *ctx = (AGSContext *)ags_alloc(config, sizeof *ctx);
    if (!ctx) return AGS_OUT_OF_MEMORY;
    if (config) ctx->cfg = *config;
    fill_device(ctx);
    if (gpuInfo) {
        gpuInfo->driverVersion = "24.30.31.03";
        gpuInfo->radeonSoftwareVersion = "24.12.1";
        gpuInfo->numDevices = 1;
        gpuInfo->devices = &ctx->device;
    }
    *context = ctx;
    return AGS_SUCCESS;
}

AMD_AGS_API AGSReturnCode agsDeInitialize(AGSContext *context) {
    if (!context) return AGS_INVALID_ARGS;
    AGSConfiguration cfg = context->cfg;
    ags_free(&cfg, context);
    return AGS_SUCCESS;
}

AMD_AGS_API AGSReturnCode agsSetDisplayMode(AGSContext *, int, int, const AGSDisplaySettings *) { return AGS_SUCCESS; }

typedef HRESULT (WINAPI *PFN_D3D12CreateDevice_t)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);

AMD_AGS_API AGSReturnCode agsDriverExtensionsDX12_CreateDevice(AGSContext *context, const AGSDX12DeviceCreationParams *cp, const AGSDX12ExtensionParams *ep, AGSDX12ReturnedParams *rp) {
    (void)ep;
    if (!context || !cp || !rp) return AGS_INVALID_ARGS;
    memset(rp, 0, sizeof *rp);
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    if (!d3d12) return AGS_MISSING_D3D_DLL;
    PFN_D3D12CreateDevice_t create = (PFN_D3D12CreateDevice_t)GetProcAddress(d3d12, "D3D12CreateDevice");
    if (!create) return AGS_MISSING_D3D_DLL;
    void *dev = NULL;
    HRESULT hr = create(cp->pAdapter, cp->FeatureLevel, cp->iid, &dev);
    if (FAILED(hr) || !dev) return AGS_DX_FAILURE;
    rp->pDevice = (ID3D12Device *)dev;
    return AGS_SUCCESS;
}

AMD_AGS_API AGSReturnCode agsDriverExtensionsDX12_CreateFromDevice(AGSContext *context, ID3D12Device *device, AGSDX12ReturnedParams *rp) {
    if (!context || !device || !rp) return AGS_INVALID_ARGS;
    memset(rp, 0, sizeof *rp);
    device->AddRef();
    rp->pDevice = device;
    return AGS_SUCCESS;
}

AMD_AGS_API AGSReturnCode agsDriverExtensionsDX12_DestroyDevice(AGSContext *context, ID3D12Device *device, unsigned int *deviceReferences) {
    if (!context || !device) return AGS_INVALID_ARGS;
    ULONG r = device->Release();
    if (deviceReferences) *deviceReferences = r;
    return AGS_SUCCESS;
}

AMD_AGS_API AGSReturnCode agsDriverExtensionsDX12_Destroy(AGSContext *context, ID3D12Device *device, unsigned int *deviceReferences) {
    return agsDriverExtensionsDX12_DestroyDevice(context, device, deviceReferences);
}

AMD_AGS_API AGSReturnCode agsDriverExtensionsDX12_PushMarker(AGSContext *, ID3D12GraphicsCommandList *, const char *) { return AGS_SUCCESS; }
AMD_AGS_API AGSReturnCode agsDriverExtensionsDX12_PopMarker(AGSContext *, ID3D12GraphicsCommandList *) { return AGS_SUCCESS; }
AMD_AGS_API AGSReturnCode agsDriverExtensionsDX12_SetMarker(AGSContext *, ID3D12GraphicsCommandList *, const char *) { return AGS_SUCCESS; }

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) { return TRUE; }
