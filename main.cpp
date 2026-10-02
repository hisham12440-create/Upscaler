// ===================================================================================
// Ahmad Hashoum - Real-Time Spatial Upscaler (EASU + RCAS + Enhance)
// DXGI Desktop Duplication -> EASU -> RCAS -> Enhance -> click-through overlay
// Build: cl /EHsc /O2 /std:c++17 /utf-8 main.cpp /link /SUBSYSTEM:WINDOWS
// ===================================================================================
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <string>
#include <chrono>
#include <algorithm>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

using Microsoft::WRL::ComPtr;

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

// true  = overlay passes mouse/keyboard through to the game (recommended)
// false = normal window (use this if you see a black screen)
static const bool kClickThrough = true;
// seconds after launch to auto-lock the active window (0 = disabled)
static const int  kAutoLockSeconds = 8;

// ===================================================================================
// 1. Shaders
// ===================================================================================
static const char* g_Shader = R"HLSL(
Texture2D<float4>   InputTex  : register(t0);
RWTexture2D<float4> OutputTex : register(u0);
SamplerState        LinSamp   : register(s0);

cbuffer Params : register(b0) {
    int4  SrcRect;
    int4  DstRect;
    uint2 OutRes;
    float RcasSharp;  // 0 = sharpest, 2 = softest (stops)
    float EnhAmt;     // Enhance strength 0..1
};

float Luma(float3 c) { return c.g + 0.5 * (c.r + c.b); }

float3 FetchSrc(int2 p) {
    p = clamp(p, SrcRect.xy, SrcRect.zw - 1);
    return InputTex.Load(int3(p, 0)).rgb;
}
float3 FetchDst(int2 p) {
    p = clamp(p, DstRect.xy, DstRect.zw - 1);
    return InputTex.Load(int3(p, 0)).rgb;
}
bool InDst(uint2 id) {
    int2 p = int2(id);
    return all(p >= DstRect.xy) && all(p < DstRect.zw);
}

// ---------------- EASU ----------------
void EasuSet(inout float2 dir, inout float len, float2 pp,
             bool biS, bool biT, bool biU, bool biV,
             float lA, float lB, float lC, float lD, float lE) {
    float w = 0.0;
    if (biS) w = (1.0 - pp.x) * (1.0 - pp.y);
    if (biT) w =        pp.x  * (1.0 - pp.y);
    if (biU) w = (1.0 - pp.x) *        pp.y;
    if (biV) w =        pp.x  *        pp.y;

    float lenX = 1.0 / max(max(abs(lD - lC), abs(lC - lB)), 1e-5);
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = saturate(abs(dirX) * lenX);
    lenX *= lenX;
    len += lenX * w;

    float lenY = 1.0 / max(max(abs(lE - lC), abs(lC - lA)), 1e-5);
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = saturate(abs(dirY) * lenY);
    lenY *= lenY;
    len += lenY * w;
}

void EasuTap(inout float3 aC, inout float aW, float2 off, float2 dir,
             float2 len, float lob, float clp, float3 c) {
    float2 v;
    v.x = off.x * dir.x + off.y * dir.y;
    v.y = off.x * (-dir.y) + off.y * dir.x;
    v *= len;
    float d2 = min(v.x * v.x + v.y * v.y, clp);
    float wB = 0.4 * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = 1.5625 * wB - 0.5625;
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

[numthreads(16, 16, 1)]
void CS_EASU(uint3 id : SV_DispatchThreadID) {
    if (id.x >= OutRes.x || id.y >= OutRes.y) return;
    if (!InDst(id.xy)) { OutputTex[id.xy] = float4(0, 0, 0, 1); return; }

    float2 srcSize = float2(SrcRect.zw - SrcRect.xy);
    float2 dstSize = float2(DstRect.zw - DstRect.xy);
    float2 scale   = srcSize / dstSize;
    float2 loc     = float2(int2(id.xy) - DstRect.xy) + 0.5;
    float2 src     = loc * scale - 0.5 + float2(SrcRect.xy);
    int2   ip      = (int2)floor(src);
    float2 pp      = src - float2(ip);

    float3 b = FetchSrc(ip + int2( 0, -1));
    float3 c = FetchSrc(ip + int2( 1, -1));
    float3 e = FetchSrc(ip + int2(-1,  0));
    float3 f = FetchSrc(ip + int2( 0,  0));
    float3 g = FetchSrc(ip + int2( 1,  0));
    float3 h = FetchSrc(ip + int2( 2,  0));
    float3 i = FetchSrc(ip + int2(-1,  1));
    float3 j = FetchSrc(ip + int2( 0,  1));
    float3 k = FetchSrc(ip + int2( 1,  1));
    float3 l = FetchSrc(ip + int2( 2,  1));
    float3 n = FetchSrc(ip + int2( 0,  2));
    float3 o = FetchSrc(ip + int2( 1,  2));

    float lB = Luma(b), lC = Luma(c), lE = Luma(e), lF = Luma(f);
    float lG = Luma(g), lH = Luma(h), lI = Luma(i), lJ = Luma(j);
    float lK = Luma(k), lL = Luma(l), lN = Luma(n), lO = Luma(o);

    float2 dir = float2(0, 0);
    float  len = 0.0;
    EasuSet(dir, len, pp, true,  false, false, false, lB, lE, lF, lG, lJ);
    EasuSet(dir, len, pp, false, true,  false, false, lC, lF, lG, lH, lK);
    EasuSet(dir, len, pp, false, false, true,  false, lF, lI, lJ, lK, lN);
    EasuSet(dir, len, pp, false, false, false, true,  lG, lJ, lK, lL, lO);

    float2 dir2 = dir * dir;
    float  dirR = dir2.x + dir2.y;
    bool   zro  = dirR < (1.0 / 32768.0);
    dirR  = rsqrt(max(dirR, 1e-12));
    dirR  = zro ? 1.0 : dirR;
    dir.x = zro ? 1.0 : dir.x;
    dir  *= dirR;

    len = len * 0.5;
    len *= len;
    float  stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));
    float2 len2    = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float  lob     = 0.5 + ((0.25 - 0.04) - 0.5) * len;
    float  clp     = 1.0 / lob;

    float3 aC = float3(0, 0, 0);
    float  aW = 0.0;
    EasuTap(aC, aW, float2( 0, -1) - pp, dir, len2, lob, clp, b);
    EasuTap(aC, aW, float2( 1, -1) - pp, dir, len2, lob, clp, c);
    EasuTap(aC, aW, float2(-1,  0) - pp, dir, len2, lob, clp, e);
    EasuTap(aC, aW, float2( 0,  0) - pp, dir, len2, lob, clp, f);
    EasuTap(aC, aW, float2( 1,  0) - pp, dir, len2, lob, clp, g);
    EasuTap(aC, aW, float2( 2,  0) - pp, dir, len2, lob, clp, h);
    EasuTap(aC, aW, float2(-1,  1) - pp, dir, len2, lob, clp, i);
    EasuTap(aC, aW, float2( 0,  1) - pp, dir, len2, lob, clp, j);
    EasuTap(aC, aW, float2( 1,  1) - pp, dir, len2, lob, clp, k);
    EasuTap(aC, aW, float2( 2,  1) - pp, dir, len2, lob, clp, l);
    EasuTap(aC, aW, float2( 0,  2) - pp, dir, len2, lob, clp, n);
    EasuTap(aC, aW, float2( 1,  2) - pp, dir, len2, lob, clp, o);

    float3 col = aC / (abs(aW) < 1e-6 ? 1e-6 : aW);
    float3 mn = min(min(f, g), min(j, k));
    float3 mx = max(max(f, g), max(j, k));
    col = clamp(col, mn, mx);

    OutputTex[id.xy] = float4(col, 1.0);
}

// ---------------- RCAS ----------------
[numthreads(16, 16, 1)]
void CS_RCAS(uint3 id : SV_DispatchThreadID) {
    if (id.x >= OutRes.x || id.y >= OutRes.y) return;
    if (!InDst(id.xy)) { OutputTex[id.xy] = float4(0, 0, 0, 1); return; }

    int2   p = int2(id.xy);
    float3 b = FetchDst(p + int2( 0, -1));
    float3 d = FetchDst(p + int2(-1,  0));
    float3 e = FetchDst(p);
    float3 f = FetchDst(p + int2( 1,  0));
    float3 h = FetchDst(p + int2( 0,  1));

    float3 mn4 = min(min(b, d), min(f, h));
    float3 mx4 = max(max(b, d), max(f, h));

    float3 hitMin = min(mn4, e) / max(4.0 * mx4, 1e-5);
    float3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, -1e-5);
    float3 lobeRGB = max(-hitMin, hitMax);
    float  lobe = max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b));

    lobe = max(-0.1875, min(lobe, 0.0)) * exp2(-RcasSharp);

    float3 res = ((b + d + f + h) * lobe + e) / (4.0 * lobe + 1.0);
    OutputTex[id.xy] = float4(saturate(res), 1.0);
}

// ---------------- Enhance (local detail boost, halo-limited) ----------------
[numthreads(16, 16, 1)]
void CS_ENH(uint3 id : SV_DispatchThreadID) {
    if (id.x >= OutRes.x || id.y >= OutRes.y) return;
    if (!InDst(id.xy)) { OutputTex[id.xy] = float4(0, 0, 0, 1); return; }

    int2   p = int2(id.xy);
    float3 c = FetchDst(p);
    float3 sum = float3(0, 0, 0);
    float3 mn = c;
    float3 mx = c;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            float3 t = FetchDst(p + int2(dx, dy));
            sum += t;
            mn = min(mn, t);
            mx = max(mx, t);
        }
    }
    float3 blur = sum / 9.0;
    float3 r = c + (c - blur) * (EnhAmt * 2.0);
    r = clamp(r, mn, mx);                       // limits halos / ringing

    float  lum = dot(r, float3(0.2126, 0.7152, 0.0722));
    r = lerp(float3(lum, lum, lum), r, 1.0 + 0.10 * EnhAmt);  // gentle vibrance
    OutputTex[id.xy] = float4(saturate(r), 1.0);
}

// ---------------- Bypass (plain bilinear) ----------------
[numthreads(16, 16, 1)]
void CS_BYPASS(uint3 id : SV_DispatchThreadID) {
    if (id.x >= OutRes.x || id.y >= OutRes.y) return;
    if (!InDst(id.xy)) { OutputTex[id.xy] = float4(0, 0, 0, 1); return; }

    float2 srcSize = float2(SrcRect.zw - SrcRect.xy);
    float2 dstSize = float2(DstRect.zw - DstRect.xy);
    float2 scale   = srcSize / dstSize;
    float2 loc     = float2(int2(id.xy) - DstRect.xy) + 0.5;
    float2 src     = loc * scale + float2(SrcRect.xy);
    src = clamp(src, float2(SrcRect.xy) + 0.5, float2(SrcRect.zw) - 0.5);

    uint w, h;
    InputTex.GetDimensions(w, h);
    float3 c = InputTex.SampleLevel(LinSamp, src / float2(w, h), 0).rgb;
    OutputTex[id.xy] = float4(c, 1.0);
}
)HLSL";

// ===================================================================================
// 2. Globals
// ===================================================================================
struct alignas(16) CBParams {
    int   src[4];
    int   dst[4];
    UINT  outW, outH;
    float sharp;
    float enh;
};

HWND g_hWnd = nullptr;   // fullscreen output window
HWND g_hHud = nullptr;   // small status window
HWND g_target = nullptr; // locked game window

ComPtr<ID3D11Device>           g_dev;
ComPtr<ID3D11DeviceContext>    g_ctx;
ComPtr<IDXGISwapChain>         g_swap;
ComPtr<IDXGIAdapter1>          g_adapter;
ComPtr<IDXGIOutput1>           g_output1;
ComPtr<IDXGIOutputDuplication> g_dup;

ComPtr<ID3D11ComputeShader> g_csEasu, g_csRcas, g_csBypass, g_csEnh;
ComPtr<ID3D11Buffer>        g_cb;
ComPtr<ID3D11SamplerState>  g_sampler;

ComPtr<ID3D11Texture2D>          g_inTex;
ComPtr<ID3D11ShaderResourceView> g_inSRV;
D3D11_TEXTURE2D_DESC             g_inDesc = {};

ComPtr<ID3D11Texture2D>           g_midTex;
ComPtr<ID3D11ShaderResourceView>  g_midSRV;
ComPtr<ID3D11UnorderedAccessView> g_midUAV;

ComPtr<ID3D11Texture2D>           g_outTex;
ComPtr<ID3D11ShaderResourceView>  g_outSRV;
ComPtr<ID3D11UnorderedAccessView> g_outUAV;

ComPtr<ID3D11Texture2D>           g_enhTex;
ComPtr<ID3D11UnorderedAccessView> g_enhUAV;

RECT g_mon = {};
UINT g_outW = 0, g_outH = 0;
RECT g_lastRect = {};

float g_sharp = 0.2f;     // lower = sharper
float g_enhAmt = 0.5f;    // Enhance strength
int   g_frames = 0;
bool  g_bypass = false;
bool  g_enhance = true;
bool  g_hudOn = true;
bool  g_dirty = true;
bool  g_needReinit = false;
bool  g_targetActive = false;
bool  g_hdr = false;
bool  g_hdrWarned = false;
int   g_dupFails = 0;
bool  g_autoLockDone = false;
ULONGLONG g_startTick = 0;

static bool Fail(const wchar_t* msg) {
    MessageBoxW(nullptr, msg, L"Error", MB_ICONERROR | MB_TOPMOST);
    return false;
}

// ===================================================================================
// 3. Monitor / adapter selection
// ===================================================================================
static bool PickOutput(IDXGIAdapter* ad, ComPtr<IDXGIOutput1>& result, bool requirePrimary) {
    POINT origin = { 0, 0 };
    HMONITOR primary = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    ComPtr<IDXGIOutput1> first;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIOutput> o;
        if (ad->EnumOutputs(i, &o) == DXGI_ERROR_NOT_FOUND) break;
        ComPtr<IDXGIOutput1> o1;
        if (FAILED(o.As(&o1))) continue;
        DXGI_OUTPUT_DESC d = {};
        o1->GetDesc(&d);
        if (!d.AttachedToDesktop) continue;
        if (d.Monitor == primary) { result = o1; return true; }
        if (!first) first = o1;
    }
    if (!requirePrimary && first) { result = first; return true; }
    return false;
}

static void UpdateMonitorInfo() {
    DXGI_OUTPUT_DESC d = {};
    g_output1->GetDesc(&d);
    g_mon  = d.DesktopCoordinates;
    g_outW = (UINT)(g_mon.right - g_mon.left);
    g_outH = (UINT)(g_mon.bottom - g_mon.top);
}

static void CheckHdr() {
    ComPtr<IDXGIOutput6> o6;
    g_hdr = false;
    if (g_output1 && SUCCEEDED(g_output1.As(&o6))) {
        DXGI_OUTPUT_DESC1 d1 = {};
        if (SUCCEEDED(o6->GetDesc1(&d1)))
            g_hdr = (d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
    }
    if (g_hdr && !g_hdrWarned) {
        g_hdrWarned = true;
        MessageBoxW(nullptr,
            L"تنبيه: الشاشة بوضع HDR.\nالبرنامج مصمم لـ SDR فقط، وقد تظهر الألوان باهتة أو خاطئة.\n"
            L"عطّل HDR من إعدادات ويندوز (Display > HDR) للحصول على ألوان صحيحة.",
            L"HDR", MB_ICONWARNING | MB_TOPMOST);
    }
}

// ===================================================================================
// 4. Window, keys
// ===================================================================================
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_DISPLAYCHANGE:
        g_needReinit = true;
        break;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        if (g_hHud) { DestroyWindow(g_hHud); g_hHud = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static bool CreateAppWindow() {
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.lpszClassName = L"AhmadHashoumUpscaler";
    if (!RegisterClassExW(&wc)) return false;

    DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    if (kClickThrough) ex |= WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;

    g_hWnd = CreateWindowExW(ex, wc.lpszClassName, L"Ahmad Hashoum Upscaler", WS_POPUP,
                             g_mon.left, g_mon.top, (int)g_outW, (int)g_outH,
                             nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hWnd) return false;

    if (kClickThrough) SetLayeredWindowAttributes(g_hWnd, 0, 255, LWA_ALPHA);

    if (!SetWindowDisplayAffinity(g_hWnd, WDA_EXCLUDEFROMCAPTURE)) {
        MessageBoxW(g_hWnd,
            L"نظامك لا يدعم استبعاد النافذة من الالتقاط (يحتاج Windows 10 الإصدار 2004 أو أحدث).\n"
            L"سترى مرآة لا نهائية.",
            L"تنبيه", MB_ICONWARNING | MB_TOPMOST);
    }
    return true;
}

static void CreateHud() {
    g_hHud = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        L"STATIC", L"", WS_POPUP | SS_LEFT | SS_CENTERIMAGE,
        g_mon.left + 12, g_mon.top + 12, 900, 26,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_hHud) return;
    SetLayeredWindowAttributes(g_hHud, 0, 225, LWA_ALPHA);
    SetWindowDisplayAffinity(g_hHud, WDA_EXCLUDEFROMCAPTURE);
    ShowWindow(g_hHud, SW_SHOWNOACTIVATE);
}

static void LockForeground() {
    HWND fg = GetForegroundWindow();
    if (fg && fg != g_hWnd && fg != g_hHud &&
        fg != GetShellWindow() && fg != GetDesktopWindow()) {
        g_target = fg;
        g_dirty = true;
    }
}

static bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Ctrl+Alt+<key>, polled (no RegisterHotKey, so nothing can block it)
//  1 lock active window   2 full desktop   3 before/after   4 Enhance on/off
//  5 sharper  6 softer    7 Enhance +      8 Enhance -      H hide HUD    Q quit
static void PollKeys() {
    static bool prev[256] = {};
    const bool mod = KeyDown(VK_CONTROL) && KeyDown(VK_MENU);
    const int keys[] = { '1','2','3','4','5','6','7','8','H','Q' };
    for (int k : keys) {
        const bool d = mod && KeyDown(k);
        if (d && !prev[k]) {
            switch (k) {
            case '1': LockForeground(); g_autoLockDone = true; break;
            case '2': g_target = nullptr; g_dirty = true; g_autoLockDone = true; break;
            case '3': g_bypass = !g_bypass; g_dirty = true; break;
            case '4': g_enhance = !g_enhance; g_dirty = true; break;
            case '5': g_sharp = std::max(0.0f, g_sharp - 0.1f); g_dirty = true; break;
            case '6': g_sharp = std::min(2.0f, g_sharp + 0.1f); g_dirty = true; break;
            case '7': g_enhAmt = std::min(1.0f, g_enhAmt + 0.1f); g_dirty = true; break;
            case '8': g_enhAmt = std::max(0.0f, g_enhAmt - 0.1f); g_dirty = true; break;
            case 'H':
                g_hudOn = !g_hudOn;
                if (g_hHud) ShowWindow(g_hHud, g_hudOn ? SW_SHOWNOACTIVATE : SW_HIDE);
                break;
            case 'Q': DestroyWindow(g_hWnd); break;
            }
        }
        prev[k] = d;
    }
}

// ===================================================================================
// 5. D3D11
// ===================================================================================
static bool CompileCS(const char* entry, ComPtr<ID3D11ComputeShader>& out) {
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3DCompile(g_Shader, strlen(g_Shader), nullptr, nullptr, nullptr,
                            entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
    if (FAILED(hr)) {
        if (err) MessageBoxA(nullptr, (const char*)err->GetBufferPointer(), "HLSL Error", MB_ICONERROR);
        return false;
    }
    return SUCCEEDED(g_dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                                nullptr, &out));
}

static bool CreateOutputTextures() {
    g_midTex.Reset(); g_midSRV.Reset(); g_midUAV.Reset();
    g_outTex.Reset(); g_outSRV.Reset(); g_outUAV.Reset();
    g_enhTex.Reset(); g_enhUAV.Reset();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = g_outW; td.Height = g_outH;
    td.MipLevels = 1; td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;

    // intermediate EASU result (FP16)
    td.Format    = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_midTex)) ||
        FAILED(g_dev->CreateShaderResourceView(g_midTex.Get(), nullptr, &g_midSRV)) ||
        FAILED(g_dev->CreateUnorderedAccessView(g_midTex.Get(), nullptr, &g_midUAV)))
        return Fail(L"فشل إنشاء Texture الوسيطة");

    // RCAS result (also read by Enhance)
    td.Format    = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_outTex)) ||
        FAILED(g_dev->CreateShaderResourceView(g_outTex.Get(), nullptr, &g_outSRV)) ||
        FAILED(g_dev->CreateUnorderedAccessView(g_outTex.Get(), nullptr, &g_outUAV)))
        return Fail(L"فشل إنشاء Texture الإخراج");

    // Enhance result
    td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_enhTex)) ||
        FAILED(g_dev->CreateUnorderedAccessView(g_enhTex.Get(), nullptr, &g_enhUAV)))
        return Fail(L"فشل إنشاء Texture التحسين");
    return true;
}

static bool InitD3D() {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return Fail(L"فشل إنشاء DXGI Factory");

    for (int pass = 0; pass < 2 && !g_output1; ++pass) {
        for (UINT i = 0; !g_output1; ++i) {
            ComPtr<IDXGIAdapter1> ad;
            if (factory->EnumAdapters1(i, &ad) == DXGI_ERROR_NOT_FOUND) break;
            if (PickOutput(ad.Get(), g_output1, pass == 0)) g_adapter = ad;
        }
    }
    if (!g_adapter || !g_output1) return Fail(L"لم أجد كرت شاشة متصل بشاشة فعّالة");

    D3D_FEATURE_LEVEL lvl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(g_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                 &lvl, 1, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx)))
        return Fail(L"فشل إنشاء جهاز D3D11");

    UpdateMonitorInfo();
    if (!CreateAppWindow()) return Fail(L"فشل إنشاء النافذة");

    ComPtr<IDXGIFactory1> adFactory;
    if (FAILED(g_adapter->GetParent(IID_PPV_ARGS(&adFactory))))
        return Fail(L"فشل الحصول على Factory الكرت");

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount       = 1;
    sd.BufferDesc.Width  = g_outW;
    sd.BufferDesc.Height = g_outH;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow      = g_hWnd;
    sd.SampleDesc.Count  = 1;
    sd.Windowed          = TRUE;
    sd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;
    if (FAILED(adFactory->CreateSwapChain(g_dev.Get(), &sd, &g_swap)))
        return Fail(L"فشل إنشاء SwapChain");
    adFactory->MakeWindowAssociation(g_hWnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);

    if (!CompileCS("CS_EASU",   g_csEasu))   return Fail(L"فشل تجميع شيدر EASU");
    if (!CompileCS("CS_RCAS",   g_csRcas))   return Fail(L"فشل تجميع شيدر RCAS");
    if (!CompileCS("CS_ENH",    g_csEnh))    return Fail(L"فشل تجميع شيدر Enhance");
    if (!CompileCS("CS_BYPASS", g_csBypass)) return Fail(L"فشل تجميع شيدر Bypass");

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth      = sizeof(CBParams);
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_dev->CreateBuffer(&cbd, nullptr, &g_cb)))
        return Fail(L"فشل إنشاء Constant Buffer");

    D3D11_SAMPLER_DESC smp = {};
    smp.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD   = D3D11_FLOAT32_MAX;
    if (FAILED(g_dev->CreateSamplerState(&smp, &g_sampler)))
        return Fail(L"فشل إنشاء Sampler");

    if (!CreateOutputTextures()) return false;

    ShowWindow(g_hWnd, kClickThrough ? SW_SHOWNOACTIVATE : SW_SHOW);
    UpdateWindow(g_hWnd);
    return true;
}

static bool ReinitOutput() {
    g_dup.Reset();

    ComPtr<IDXGIOutput1> o;
    if (!PickOutput(g_adapter.Get(), o, false)) return false;
    g_output1 = o;

    const UINT oldW = g_outW, oldH = g_outH;
    UpdateMonitorInfo();

    SetWindowPos(g_hWnd, HWND_TOPMOST, g_mon.left, g_mon.top, (int)g_outW, (int)g_outH,
                 SWP_NOACTIVATE);
    if (g_hHud) SetWindowPos(g_hHud, HWND_TOPMOST, g_mon.left + 12, g_mon.top + 12, 0, 0,
                             SWP_NOSIZE | SWP_NOACTIVATE);

    if (g_outW != oldW || g_outH != oldH) {
        g_ctx->ClearState();
        g_ctx->Flush();
        if (FAILED(g_swap->ResizeBuffers(0, g_outW, g_outH, DXGI_FORMAT_UNKNOWN, 0))) return false;
        if (!CreateOutputTextures()) return false;
    }

    g_inTex.Reset(); g_inSRV.Reset(); g_inDesc = {};
    g_dirty = true;
    CheckHdr();
    return true;
}

// ===================================================================================
// 6. Capture
// ===================================================================================
static bool InitDuplication() {
    g_dup.Reset();
    return SUCCEEDED(g_output1->DuplicateOutput(g_dev.Get(), &g_dup));
}

static bool EnsureInputTexture(const D3D11_TEXTURE2D_DESC& src) {
    if (g_inTex && src.Width == g_inDesc.Width &&
        src.Height == g_inDesc.Height && src.Format == g_inDesc.Format)
        return true;

    g_inTex.Reset();
    g_inSRV.Reset();

    D3D11_TEXTURE2D_DESC d = {};
    d.Width = src.Width; d.Height = src.Height;
    d.MipLevels = 1; d.ArraySize = 1;
    d.Format = src.Format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_inTex)) ||
        FAILED(g_dev->CreateShaderResourceView(g_inTex.Get(), nullptr, &g_inSRV))) {
        g_inTex.Reset(); g_inSRV.Reset();
        return false;
    }
    g_inDesc = d;
    return true;
}

static void ComputeSrcRect(RECT& r) {
    const LONG tw = (LONG)g_inDesc.Width, th = (LONG)g_inDesc.Height;
    r.left = 0; r.top = 0; r.right = tw; r.bottom = th;
    g_targetActive = false;
    if (!g_target) return;

    if (!IsWindow(g_target) || IsIconic(g_target)) { g_target = nullptr; return; }

    RECT cr = {};
    POINT p = { 0, 0 };
    if (!GetClientRect(g_target, &cr) || !ClientToScreen(g_target, &p)) return;

    RECT w;
    w.left   = p.x - g_mon.left;
    w.top    = p.y - g_mon.top;
    w.right  = w.left + cr.right;
    w.bottom = w.top + cr.bottom;

    w.left   = std::clamp<LONG>(w.left,   0, tw);
    w.right  = std::clamp<LONG>(w.right,  0, tw);
    w.top    = std::clamp<LONG>(w.top,    0, th);
    w.bottom = std::clamp<LONG>(w.bottom, 0, th);

    if (w.right - w.left < 32 || w.bottom - w.top < 32) return;
    r = w;
    g_targetActive = true;
}

// ===================================================================================
// 7. Processing & presenting
// ===================================================================================
static void RunPass(ID3D11ComputeShader* cs, ID3D11ShaderResourceView* srv,
                    ID3D11UnorderedAccessView* uav) {
    ID3D11UnorderedAccessView* nullUAV = nullptr;
    ID3D11ShaderResourceView*  nullSRV = nullptr;
    g_ctx->CSSetShader(cs, nullptr, 0);
    g_ctx->CSSetShaderResources(0, 1, &srv);
    g_ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    g_ctx->Dispatch((g_outW + 15) / 16, (g_outH + 15) / 16, 1);
    g_ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
    g_ctx->CSSetShaderResources(0, 1, &nullSRV);
}

static void RunUpscale(const RECT& src) {
    const int sw = (int)(src.right - src.left);
    const int sh = (int)(src.bottom - src.top);
    if (sw <= 0 || sh <= 0) return;

    const double s = std::min((double)g_outW / sw, (double)g_outH / sh);
    const int dw = std::min<int>((int)g_outW, (int)std::lround(sw * s));
    const int dh = std::min<int>((int)g_outH, (int)std::lround(sh * s));
    const int dx = ((int)g_outW - dw) / 2;
    const int dy = ((int)g_outH - dh) / 2;

    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(g_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    CBParams p = {};
    p.src[0] = (int)src.left;  p.src[1] = (int)src.top;
    p.src[2] = (int)src.right; p.src[3] = (int)src.bottom;
    p.dst[0] = dx;             p.dst[1] = dy;
    p.dst[2] = dx + dw;        p.dst[3] = dy + dh;
    p.outW = g_outW;           p.outH = g_outH;
    p.sharp = g_sharp;
    p.enh = g_enhAmt;
    memcpy(m.pData, &p, sizeof(p));
    g_ctx->Unmap(g_cb.Get(), 0);

    ID3D11Buffer*       cb   = g_cb.Get();
    ID3D11SamplerState* samp = g_sampler.Get();
    g_ctx->CSSetConstantBuffers(0, 1, &cb);
    g_ctx->CSSetSamplers(0, 1, &samp);

    ID3D11Texture2D* finalTex = g_outTex.Get();
    if (g_bypass) {
        RunPass(g_csBypass.Get(), g_inSRV.Get(), g_outUAV.Get());
    } else {
        RunPass(g_csEasu.Get(), g_inSRV.Get(), g_midUAV.Get());
        RunPass(g_csRcas.Get(), g_midSRV.Get(), g_outUAV.Get());
        if (g_enhance) {
            RunPass(g_csEnh.Get(), g_outSRV.Get(), g_enhUAV.Get());
            finalTex = g_enhTex.Get();
        }
    }

    ComPtr<ID3D11Texture2D> bb;
    if (SUCCEEDED(g_swap->GetBuffer(0, IID_PPV_ARGS(&bb)))) {
        g_ctx->CopyResource(bb.Get(), finalTex);
        bb.Reset();
        HRESULT hr = g_swap->Present(0, 0);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            MessageBoxW(nullptr, L"تم فقدان جهاز الرسوميات (تحديث درايفر أو تعليق GPU).\nأعد تشغيل البرنامج.",
                        L"Error", MB_ICONERROR | MB_TOPMOST);
            DestroyWindow(g_hWnd);
            return;
        }
        ++g_frames;
    }
    g_lastRect = src;
    g_dirty = false;
}

static void ProcessFrame() {
    if (!g_dup) {
        static ULONGLONG lastTry = 0;
        ULONGLONG now = GetTickCount64();
        if (now - lastTry < 250) { Sleep(10); return; }
        lastTry = now;
        if (!InitDuplication()) {
            if (++g_dupFails >= 4) { g_dupFails = 0; g_needReinit = true; }
            return;
        }
        g_dupFails = 0;
    }

    DXGI_OUTDUPL_FRAME_INFO info = {};
    ComPtr<IDXGIResource> res;
    HRESULT hr = g_dup->AcquireNextFrame(16, &info, &res);
    bool newImage = false;

    if (hr == S_OK) {
        if (info.LastPresentTime.QuadPart != 0 || info.AccumulatedFrames > 0) {
            ComPtr<ID3D11Texture2D> frame;
            if (SUCCEEDED(res.As(&frame))) {
                D3D11_TEXTURE2D_DESC d = {};
                frame->GetDesc(&d);
                if (EnsureInputTexture(d)) {
                    g_ctx->CopyResource(g_inTex.Get(), frame.Get());
                    newImage = true;
                }
            }
        }
        res.Reset();
        g_dup->ReleaseFrame();
    } else if (hr != DXGI_ERROR_WAIT_TIMEOUT) {
        g_dup.Reset();
        g_needReinit = true;
        return;
    }

    if (!g_inSRV) return;

    RECT cur;
    ComputeSrcRect(cur);
    const bool rectChanged = !EqualRect(&cur, &g_lastRect);
    if (newImage || rectChanged || g_dirty) RunUpscale(cur);
}

// ===================================================================================
// 8. Main
// ===================================================================================
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    SetProcessDPIAware();

    if (!InitD3D()) return 0;
    CreateHud();
    CheckHdr();

    MSG msg = {};
    g_startTick = GetTickCount64();
    auto lastHud = std::chrono::steady_clock::now();
    ULONGLONG lastTop = 0, lastReinit = 0;

    while (msg.message != WM_QUIT) {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            continue;
        }

        const ULONGLONG tick = GetTickCount64();

        PollKeys();

        // auto-lock the active (game) window a few seconds after launch
        if (!g_autoLockDone && kAutoLockSeconds > 0 &&
            tick - g_startTick >= (ULONGLONG)kAutoLockSeconds * 1000ULL) {
            g_autoLockDone = true;
            LockForeground();
        }

        if (g_needReinit && tick - lastReinit > 500) {
            lastReinit = tick;
            g_needReinit = !ReinitOutput();
        }

        ProcessFrame();

        if (tick - lastTop > 1000) {
            lastTop = tick;
            SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            if (g_hHud && g_hudOn)
                SetWindowPos(g_hHud, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }

        auto now = std::chrono::steady_clock::now();
        float ms = std::chrono::duration<float, std::milli>(now - lastHud).count();
        if (ms >= 500.0f) {
            float fps = g_frames * 1000.0f / ms;
            g_frames = 0;
            lastHud = now;

            int lockIn = 0;
            if (!g_autoLockDone && kAutoLockSeconds > 0) {
                long long left = (long long)kAutoLockSeconds - (long long)((tick - g_startTick) / 1000ULL);
                lockIn = (int)std::max<long long>(0, left);
            }

            wchar_t lockTxt[48] = L"";
            if (lockIn > 0) swprintf_s(lockTxt, L"  |  AUTO-LOCK in %ds", lockIn);

            wchar_t buf[400];
            swprintf_s(buf, L"  Src %ldx%ld > Out %ux%u  |  FPS %d  |  Sharp %.1f  |  %s%s  |  %s%s%s%s",
                       (long)(g_lastRect.right - g_lastRect.left),
                       (long)(g_lastRect.bottom - g_lastRect.top),
                       g_outW, g_outH, (int)(fps + 0.5f), (double)g_sharp,
                       g_bypass ? L"BYPASS" : L"EASU+RCAS",
                       (!g_bypass && g_enhance) ? L"+ENHANCE" : L"",
                       g_targetActive ? L"WINDOW" : L"DESKTOP",
                       g_dup ? L"" : L"  |  NO CAPTURE",
                       g_hdr ? L"  |  HDR!" : L"",
                       lockTxt);
            if (g_hHud) SetWindowTextW(g_hHud, buf);
            SetWindowTextW(g_hWnd, buf);
        }
    }

    if (g_ctx) g_ctx->ClearState();
    return (int)msg.wParam;
}
