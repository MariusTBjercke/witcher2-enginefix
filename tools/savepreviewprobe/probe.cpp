// Isolated reproduction of FUN_0053CEF0's readback/downsample/BMP path.
// Loads an explicitly named D3D9 implementation; never loads or writes saves.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>

struct Buffer : IUnknown {
    virtual void* WINAPI GetBufferPointer() = 0;
    virtual DWORD WINAPI GetBufferSize() = 0;
};
typedef IDirect3D9* (WINAPI *Create9)(UINT);
typedef HRESULT (WINAPI *SaveMemory)(Buffer**, int, IDirect3DSurface9*, const PALETTEENTRY*, const RECT*);
typedef HRESULT (WINAPI *SaveFile)(const wchar_t*, int, IDirect3DSurface9*, const PALETTEENTRY*, const RECT*);

static void check(const char* label, HRESULT hr) {
    printf("%-38s HRESULT=%08lX\n", label, (unsigned long)hr);
    if (FAILED(hr)) exit(1);
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 5) {
        puts("Usage: probe.exe <absolute d3d9.dll> <width> <height> <output.bmp>");
        return 2;
    }
    UINT width = (UINT)_wtoi(argv[2]), height = (UINT)_wtoi(argv[3]);
    if (width < 640 || height < 360 || width > 7680 || height > 4320) return 2;
    HMODULE module = LoadLibraryExW(argv[1], NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) { printf("LoadLibrary D3D9 failed: %lu\n", GetLastError()); return 1; }
    HMODULE codec = LoadLibraryW(L"d3dx9_39.dll"); // game's actual import
    if (!codec) { printf("LoadLibrary D3DX failed: %lu\n", GetLastError()); return 1; }
    Create9 create = (Create9)GetProcAddress(module, "Direct3DCreate9");
    SaveMemory saveMemory = (SaveMemory)GetProcAddress(codec, "D3DXSaveSurfaceToFileInMemory");
    SaveFile saveFile = (SaveFile)GetProcAddress(codec, "D3DXSaveSurfaceToFileW");
    if (!create || !saveMemory || !saveFile) return 1;
    IDirect3D9* api = create(D3D_SDK_VERSION);
    if (!api) return 1;
    // Hidden window: a device is needed, but no desktop capture or input is used.
    HWND window = CreateWindowExW(0, L"STATIC", L"Save preview probe", WS_POPUP,
                                  0, 0, 640, 360, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!window) return 1;
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = window;
    pp.BackBufferWidth = width;
    pp.BackBufferHeight = height;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* device = NULL;
    check("CreateDevice", api->CreateDevice(0, D3DDEVTYPE_HAL, window,
          D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device));
    IDirect3DSurface9 *back = NULL, *source = NULL, *target = NULL;
    check("GetBackBuffer", device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back));
    D3DSURFACE_DESC desc = {};
    check("GetDesc", back->GetDesc(&desc));
    printf("source=%ux%u format=%u samples=%u\n", desc.Width, desc.Height, desc.Format, desc.MultiSampleType);
    const DWORD expected = D3DCOLOR_XRGB(37, 113, 191);
    check("Clear known test color", device->Clear(0, NULL, D3DCLEAR_TARGET, expected, 1.0f, 0));
    check("CreateOffscreenPlainSurface SYSTEMMEM", device->CreateOffscreenPlainSurface(
          desc.Width, desc.Height, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &source, NULL));
    check("GetRenderTargetData", device->GetRenderTargetData(back, source));
    check("CreateOffscreenPlainSurface SCRATCH", device->CreateOffscreenPlainSurface(
          640, 360, D3DFMT_X8R8G8B8, D3DPOOL_SCRATCH, &target, NULL));
    D3DLOCKED_RECT src = {}, dst = {};
    RECT rect = {0, 0, (LONG)width, (LONG)height};
    check("LockRect source (game flags)", source->LockRect(&src, &rect,
          D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK | D3DLOCK_NO_DIRTY_UPDATE));
    check("LockRect target (game flags)", target->LockRect(&dst, NULL, D3DLOCK_NOSYSLOCK));
    // Same integer block averaging and fractional inner offsets as the engine.
    const UINT nx = width / 640, ny = height / 360, count = nx * ny;
    const float fx = 640.0f / width, fy = 360.0f / height;
    unsigned mismatches = 0;
    for (UINT y = 0; y < 360; ++y) {
        DWORD* row = (DWORD*)((BYTE*)dst.pBits + y * dst.Pitch);
        for (UINT x = 0; x < 640; ++x) {
            UINT b = 0, g = 0, r = 0;
            float oy = 0.0f, ox = 0.0f;
            for (UINT j = 0; j < ny; ++j, oy += fy) {
                const BYTE* in = (BYTE*)src.pBits + ((y * height / 360) + (UINT)oy) * src.Pitch;
                for (UINT i = 0; i < nx; ++i, ox += fx) {
                    const BYTE* pixel = in + ((x * width / 640) + (UINT)ox) * 4;
                    b += pixel[0]; g += pixel[1]; r += pixel[2];
                }
            }
            row[x] = 0xFF000000 | ((r/count) << 16) | ((g/count) << 8) | (b/count);
            if ((row[x] & 0xFFFFFF) != (expected & 0xFFFFFF)) ++mismatches;
        }
    }
    check("UnlockRect source", source->UnlockRect());
    check("UnlockRect target", target->UnlockRect());
    printf("downsample pixel mismatches=%u / 230400\n", mismatches);
    Buffer* buffer = NULL;
    check("D3DXSaveSurfaceToFileInMemory BMP", saveMemory(&buffer, 0, target, NULL, NULL));
    const BYTE* bytes = (const BYTE*)buffer->GetBufferPointer();
    DWORD size = buffer->GetBufferSize();
    printf("encoded BMP: size=%lu magic=%c%c\n", size, bytes[0], bytes[1]);
    bool valid = size > 54 && bytes[0] == 'B' && bytes[1] == 'M';
    check("D3DXSaveSurfaceToFileW BMP", saveFile(argv[4], 0, target, NULL, NULL));
    buffer->Release(); target->Release(); source->Release(); back->Release();
    device->Release(); api->Release(); DestroyWindow(window);
    FreeLibrary(codec); FreeLibrary(module);
    return (mismatches == 0 && valid) ? 0 : 1;
}
