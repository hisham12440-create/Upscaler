// OLED Upscaler v2 - spatial upscaler + OLED output stage (Windows 10/11, D3D11)
//
// Pipeline:
//   Desktop Duplication -> Pass1: Lanczos2 / Bicubic (anti-ringing)
//                       -> Pass2: CAS sharpen + OLED stage (true black, saturation, dither)
//                       -> Flip-model swapchain (tearing = low latency)
//
// Hotkeys (Ctrl+Alt+...):
//   F9 start/stop | Left/Right sharpness | Down/Up saturation | O OLED stage | E engine | F10 quit

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <string>
#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")

using Microsoft::WRL::ComPtr;

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

enum { HK_TOGGLE = 1, HK_SHARP_DN, HK_SHARP_UP, HK_SAT_DN, HK_SAT_UP, HK_OLED, HK_ENGINE, HK_QUIT };

static const char* kShader = R"HLSL(
cbuffer CB : register(b0)
{
    float2 srcSize;
    float2 dstSize;
    float  sharp;
    float  sat;
    float  blackFloor;
    float  frame;
    float  oled;
    float3 pad;
};

Texture2D tex : register(t0);
SamplerState lin : register(s0);

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VS(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}

// ---------- Pass 1a: Catmull-Rom bicubic (5 bilinear taps) ----------
float4 PS_Bicubic(VSOut i) : SV_Target
{
    float2 pos = i.uv * srcSize;
    float2 c = floor(pos - 0.5) + 0.5;
    float2 f = pos - c;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 off = w2 / w12;

    float2 t0  = (c - 1.0) / srcSize;
    float2 t12 = (c + off) / srcSize;
    float2 t3  = (c + 2.0) / srcSize;

    float3 r = 0;
    r += tex.SampleLevel(lin, float2(t12.x, t0.y), 0).rgb  * (w12.x * w0.y);
    r += tex.SampleLevel(lin, float2(t0.x, t12.y), 0).rgb  * (w0.x * w12.y);
    r += tex.SampleLevel(lin, float2(t12.x, t12.y), 0).rgb * (w12.x * w12.y);
    r += tex.SampleLevel(lin, float2(t3.x, t12.y), 0).rgb  * (w3.x * w12.y);
    r += tex.SampleLevel(lin, float2(t12.x, t3.y), 0).rgb  * (w12.x * w3.y);
    float ws = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return float4(saturate(r / ws), 1);
}

// ---------- Pass 1b: Lanczos-2 (16 taps) with anti-ringing ----------
float L2(float x)
{
    x = abs(x);
    if (x < 1e-4) return 1.0;
    if (x >= 2.0) return 0.0;
    float px = 3.14159265 * x;
    return 2.0 * sin(px) * sin(px * 0.5) / (px * px);
}

float4 PS_Lanczos(VSOut i) : SV_Target
{
    float2 p = i.uv * srcSize - 0.5;
    int2 b = (int2)floor(p);
    float2 f = p - b;
    int2 lim = (int2)srcSize - 1;

    float wx[4], wy[4];
    [unroll] for (int k = 0; k < 4; k++) { wx[k] = L2(k - 1 - f.x); wy[k] = L2(k - 1 - f.y); }

    float3 acc = 0; float ws = 0;
    float3 imn = 1e5, imx = -1e5;
    [unroll] for (int y = 0; y < 4; y++)
    {
        [unroll] for (int x = 0; x < 4; x++)
        {
            int2 q = clamp(b + int2(x - 1, y - 1), int2(0, 0), lim);
            float3 s = tex.Load(int3(q, 0)).rgb;
            float w = wx[x] * wy[y];
            acc += s * w; ws += w;
            if (x >= 1 && x <= 2 && y >= 1 && y <= 2) { imn = min(imn, s); imx = max(imx, s); }
        }
    }
    float3 r = acc / ws;
    float3 rc = clamp(r, imn, imx);     // de-ringing (blended to keep crispness)
    r = lerp(rc, r, 0.35);
    return float4(saturate(r), 1);
}

// ---------- Pass 2: CAS sharpen + OLED stage + dither ----------
float3 T(int2 q)
{
    int2 lim = (int2)dstSize - 1;
    return tex.Load(int3(clamp(q, int2(0, 0), lim), 0)).rgb;
}

float IGN(float2 p, float fr)
{
    p += 5.588238 * fr;
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

float4 PS_Final(VSOut i) : SV_Target
{
    int2 p = (int2)i.pos.xy;

    float3 a = T(p + int2(-1, -1));
    float3 b = T(p + int2( 0, -1));
    float3 c = T(p + int2( 1, -1));
    float3 d = T(p + int2(-1,  0));
    float3 e = T(p);
    float3 f = T(p + int2( 1,  0));
    float3 g = T(p + int2(-1,  1));
    float3 h = T(p + int2( 0,  1));
    float3 k = T(p + int2( 1,  1));

    // Contrast Adaptive Sharpening (FidelityFX CAS idea)
    float3 mn = min(min(min(d, e), min(f, b)), h);
    float3 mn2 = min(min(a, c), min(g, k));
    mn += min(mn, mn2);
    float3 mx = max(max(max(d, e), max(f, b)), h);
    float3 mx2 = max(max(a, c), max(g, k));
    mx += max(mx, mx2);

    float3 amp = saturate(min(mn, 2.0 - mx) / max(mx, 1e-5));
    amp = sqrt(amp);
    float peak = -1.0 / lerp(8.0, 5.0, saturate(sharp));
    float3 w = (sharp > 0.001) ? amp * peak : 0.0;
    float3 col = saturate(((b + d + f + h) * w + e) / (1.0 + 4.0 * w));

    // OLED stage
    if (oled > 0.5)
    {
        col = max(col - blackFloor, 0.0) / (1.0 - blackFloor);   // true black
        float l = dot(col, float3(0.2126, 0.7152, 0.0722));
        col = saturate(lerp(l.xxx, col, sat));                   // OLED saturation
    }

    // Anti-banding dither (skipped on pure black so OLED pixels stay off)
    float fr = fmod(frame, 64.0);
    float n = IGN((float2)p, fr) + IGN((float2)p, fr + 1.0) - 1.0;  // triangular
    float da = smoothstep(0.002, 0.02, max(col.r, max(col.g, col.b)));
    col = saturate(col + n * da / 255.0);

    return float4(col, 1);
}
)HLSL";

struct CBData
{
    float srcW, srcH, dstW, dstH;
    float sharp, sat, blackFloor, frame;
    float oled, p0, p1, p2;
};

// ---- globals ----
static HWND g_hwnd = nullptr;
static ComPtr<IDXGIFactory2> g_factory;
static ComPtr<IDXGIOutput1> g_out1;
static ComPtr<ID3D11Device> g_dev;
static ComPtr<ID3D11DeviceContext> g_ctx;
static ComPtr<IDXGISwapChain1> g_sc;
static ComPtr<ID3D11RenderTargetView> g_rtv;
static ComPtr<IDXGIOutputDuplication> g_dup;
static ComPtr<ID3D11Texture2D> g_srcTex;
static ComPtr<ID3D11ShaderResourceView> g_srv;
static ComPtr<ID3D11Texture2D> g_midTex;
static ComPtr<ID3D11RenderTargetView> g_midRtv;
static ComPtr<ID3D11ShaderResourceView> g_midSrv;
static ComPtr<ID3D11VertexShader> g_vs;
static ComPtr<ID3D11PixelShader> g_psLanczos, g_psBicubic, g_psFinal;
static ComPtr<ID3D11Buffer> g_cb;
static ComPtr<ID3D11SamplerState> g_samp;

static RECT g_mon{};
static UINT g_W = 0, g_H = 0;
static BOOL g_tearing = FALSE;
static bool g_active = false;
static RECT g_src{};
static UINT g_srcW = 0, g_srcH = 0;
static UINT g_frame = 0;

// settings (saved to ini)
static float g_sharp = 0.5f;
static float g_sat = 1.10f;
static int g_engine = 0;     // 0 = Lanczos2, 1 = Bicubic
static int g_oled = 1;
static const float kBlackFloor = 0.003f;

// ---- config ----
static std::wstring IniPath()
{
    wchar_t b[MAX_PATH];
    GetModuleFileNameW(nullptr, b, MAX_PATH);
    std::wstring s = b;
    size_t p = s.find_last_of(L'.');
    if (p != std::wstring::npos) s = s.substr(0, p);
    return s + L".ini";
}

static void LoadCfg()
{
    std::wstring ini = IniPath();
    g_sharp = GetPrivateProfileIntW(L"cfg", L"sharp", 50, ini.c_str()) / 100.0f;
    g_sat = GetPrivateProfileIntW(L"cfg", L"sat", 110, ini.c_str()) / 100.0f;
    g_engine = GetPrivateProfileIntW(L"cfg", L"engine", 0, ini.c_str());
    g_oled = GetPrivateProfileIntW(L"cfg", L"oled", 1, ini.c_str());
    g_sharp = std::clamp(g_sharp, 0.0f, 1.0f);
    g_sat = std::clamp(g_sat, 0.5f, 1.6f);
    g_engine = g_engine ? 1 : 0;
    g_oled = g_oled ? 1 : 0;
}

static void SaveCfg()
{
    std::wstring ini = IniPath();
    wchar_t v[16];
    wsprintfW(v, L"%d", (int)(g_sharp * 100.0f + 0.5f)); WritePrivateProfileStringW(L"cfg", L"sharp", v, ini.c_str());
    wsprintfW(v, L"%d", (int)(g_sat * 100.0f + 0.5f));   WritePrivateProfileStringW(L"cfg", L"sat", v, ini.c_str());
    wsprintfW(v, L"%d", g_engine);                       WritePrivateProfileStringW(L"cfg", L"engine", v, ini.c_str());
    wsprintfW(v, L"%d", g_oled);                         WritePrivateProfileStringW(L"cfg", L"oled", v, ini.c_str());
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static bool CreateDuplication()
{
    g_dup.Reset();
    return SUCCEEDED(g_out1->DuplicateOutput(g_dev.Get(), &g_dup));
}

static bool Compile(const char* entry, const char* target, ComPtr<ID3DBlob>& blob)
{
    ComPtr<ID3DBlob> err;
    HRESULT hr = D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr,
        entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
    if (FAILED(hr) && err)
        MessageBoxA(nullptr, (const char*)err->GetBufferPointer(), "Shader error", MB_ICONERROR);
    return SUCCEEDED(hr);
}

static bool InitD3D(HINSTANCE hInst)
{
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&g_factory)))) return false;

    // Desktop Duplication only works on the adapter that owns the output.
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    for (UINT i = 0; g_factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        if (SUCCEEDED(adapter->EnumOutputs(0, &output))) break;
        adapter.Reset();
    }
    if (!adapter || !output) return false;

    DXGI_OUTPUT_DESC od{};
    output->GetDesc(&od);
    g_mon = od.DesktopCoordinates;
    g_W = g_mon.right - g_mon.left;
    g_H = g_mon.bottom - g_mon.top;

    if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &g_dev, nullptr, &g_ctx))) return false;

    output.As(&g_out1);
    if (!g_out1 || !CreateDuplication()) return false;

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"OledUpscalerWnd";
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        wc.lpszClassName, L"OLED Upscaler", WS_POPUP,
        g_mon.left, g_mon.top, g_W, g_H, nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return false;
    SetLayeredWindowAttributes(g_hwnd, 0, 255, LWA_ALPHA);
    SetWindowDisplayAffinity(g_hwnd, WDA_EXCLUDEFROMCAPTURE);   // no feedback loop

    ComPtr<IDXGIFactory5> f5;
    if (SUCCEEDED(g_factory.As(&f5)))
        f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &g_tearing, sizeof(g_tearing));

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = g_W; sd.Height = g_H;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (FAILED(g_factory->CreateSwapChainForHwnd(g_dev.Get(), g_hwnd, &sd, nullptr, nullptr, &g_sc)))
        return false;
    g_factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);

    ComPtr<ID3D11Texture2D> bb;
    g_sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_dev->CreateRenderTargetView(bb.Get(), nullptr, &g_rtv);

    // Intermediate FP16 target (keeps precision between upscale and sharpen/dither)
    D3D11_TEXTURE2D_DESC md{};
    md.Width = g_W; md.Height = g_H;
    md.MipLevels = 1; md.ArraySize = 1;
    md.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    md.SampleDesc.Count = 1;
    md.Usage = D3D11_USAGE_DEFAULT;
    md.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_dev->CreateTexture2D(&md, nullptr, &g_midTex))) return false;
    g_dev->CreateRenderTargetView(g_midTex.Get(), nullptr, &g_midRtv);
    g_dev->CreateShaderResourceView(g_midTex.Get(), nullptr, &g_midSrv);

    ComPtr<ID3DBlob> vsb, lb, bbk, fb;
    if (!Compile("VS", "vs_5_0", vsb)) return false;
    if (!Compile("PS_Lanczos", "ps_5_0", lb)) return false;
    if (!Compile("PS_Bicubic", "ps_5_0", bbk)) return false;
    if (!Compile("PS_Final", "ps_5_0", fb)) return false;
    g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
    g_dev->CreatePixelShader(lb->GetBufferPointer(), lb->GetBufferSize(), nullptr, &g_psLanczos);
    g_dev->CreatePixelShader(bbk->GetBufferPointer(), bbk->GetBufferSize(), nullptr, &g_psBicubic);
    g_dev->CreatePixelShader(fb->GetBufferPointer(), fb->GetBufferSize(), nullptr, &g_psFinal);

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(CBData);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g_dev->CreateBuffer(&bd, nullptr, &g_cb);

    D3D11_SAMPLER_DESC sm{};
    sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    g_dev->CreateSamplerState(&sm, &g_samp);
    return true;
}

static bool StartScaling()
{
    HWND fg = GetForegroundWindow();
    if (!fg || fg == g_hwnd || fg == GetDesktopWindow() || fg == GetShellWindow()) return false;

    RECT cr{};
    GetClientRect(fg, &cr);
    POINT tl{ cr.left, cr.top }, br{ cr.right, cr.bottom };
    ClientToScreen(fg, &tl);
    ClientToScreen(fg, &br);

    RECT r{ tl.x, tl.y, br.x, br.y };
    IntersectRect(&g_src, &r, &g_mon);
    g_srcW = g_src.right - g_src.left;
    g_srcH = g_src.bottom - g_src.top;
    if (g_srcW < 64 || g_srcH < 64) return false;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = g_srcW; td.Height = g_srcH;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    g_srv.Reset(); g_srcTex.Reset();
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_srcTex))) return false;
    if (FAILED(g_dev->CreateShaderResourceView(g_srcTex.Get(), nullptr, &g_srv))) return false;

    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(g_hwnd, HWND_TOPMOST, g_mon.left, g_mon.top, g_W, g_H, SWP_NOACTIVATE);
    g_active = true;
    return true;
}

static void StopScaling()
{
    g_active = false;
    ShowWindow(g_hwnd, SW_HIDE);
}

static void RenderFrame()
{
    ComPtr<IDXGIResource> res;
    DXGI_OUTDUPL_FRAME_INFO fi{};
    HRESULT hr = g_dup->AcquireNextFrame(8, &fi, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return;
    if (hr == DXGI_ERROR_ACCESS_LOST) { CreateDuplication(); return; }
    if (FAILED(hr)) return;

    ComPtr<ID3D11Texture2D> frame;
    res.As(&frame);
    if (frame)
    {
        D3D11_BOX box{};
        box.left = g_src.left - g_mon.left;
        box.top = g_src.top - g_mon.top;
        box.right = box.left + g_srcW;
        box.bottom = box.top + g_srcH;
        box.front = 0; box.back = 1;
        g_ctx->CopySubresourceRegion(g_srcTex.Get(), 0, 0, 0, 0, frame.Get(), 0, &box);
    }
    g_dup->ReleaseFrame();

    CBData cb{ (float)g_srcW, (float)g_srcH, (float)g_W, (float)g_H,
               g_sharp, g_sat, kBlackFloor, (float)(g_frame++),
               (float)g_oled, 0, 0, 0 };
    g_ctx->UpdateSubresource(g_cb.Get(), 0, nullptr, &cb, 0, 0);

    ID3D11SamplerState* ss = g_samp.Get();
    ID3D11Buffer* cbuf = g_cb.Get();
    ID3D11ShaderResourceView* nullSrv = nullptr;
    const float black[4] = { 0, 0, 0, 1 };

    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_vs.Get(), nullptr, 0);
    g_ctx->PSSetSamplers(0, 1, &ss);
    g_ctx->PSSetConstantBuffers(0, 1, &cbuf);

    // ---- Pass 1: upscale into FP16 intermediate (letterboxed, aspect kept) ----
    g_ctx->PSSetShaderResources(0, 1, &nullSrv);
    ID3D11RenderTargetView* mid = g_midRtv.Get();
    g_ctx->OMSetRenderTargets(1, &mid, nullptr);
    g_ctx->ClearRenderTargetView(mid, black);

    float scale = std::min((float)g_W / g_srcW, (float)g_H / g_srcH);
    float vw = g_srcW * scale, vh = g_srcH * scale;
    D3D11_VIEWPORT vp1{ (g_W - vw) * 0.5f, (g_H - vh) * 0.5f, vw, vh, 0.f, 1.f };
    g_ctx->RSSetViewports(1, &vp1);
    g_ctx->PSSetShader(g_engine == 0 ? g_psLanczos.Get() : g_psBicubic.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srv = g_srv.Get();
    g_ctx->PSSetShaderResources(0, 1, &srv);
    g_ctx->Draw(3, 0);

    // ---- Pass 2: CAS + OLED stage + dither into backbuffer ----
    g_ctx->PSSetShaderResources(0, 1, &nullSrv);
    ID3D11RenderTargetView* bbRtv = g_rtv.Get();
    g_ctx->OMSetRenderTargets(1, &bbRtv, nullptr);
    D3D11_VIEWPORT vp2{ 0.f, 0.f, (float)g_W, (float)g_H, 0.f, 1.f };
    g_ctx->RSSetViewports(1, &vp2);
    g_ctx->PSSetShader(g_psFinal.Get(), nullptr, 0);
    ID3D11ShaderResourceView* msrv = g_midSrv.Get();
    g_ctx->PSSetShaderResources(0, 1, &msrv);
    g_ctx->Draw(3, 0);

    g_sc->Present(0, g_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    SetProcessDPIAware();
    LoadCfg();

    if (!InitD3D(hInst))
    {
        MessageBoxW(nullptr,
            L"Init failed.\nOn hybrid laptops the screen must be driven by the GPU running this app.\n"
            L"Also make sure no other screen-capture app is running.",
            L"OLED Upscaler", MB_ICONERROR);
        return 1;
    }

    const UINT mod = MOD_CONTROL | MOD_ALT;
    RegisterHotKey(nullptr, HK_TOGGLE, mod | MOD_NOREPEAT, VK_F9);
    RegisterHotKey(nullptr, HK_SHARP_DN, mod, VK_LEFT);
    RegisterHotKey(nullptr, HK_SHARP_UP, mod, VK_RIGHT);
    RegisterHotKey(nullptr, HK_SAT_DN, mod, VK_DOWN);
    RegisterHotKey(nullptr, HK_SAT_UP, mod, VK_UP);
    RegisterHotKey(nullptr, HK_OLED, mod | MOD_NOREPEAT, 'O');
    RegisterHotKey(nullptr, HK_ENGINE, mod | MOD_NOREPEAT, 'E');
    RegisterHotKey(nullptr, HK_QUIT, mod | MOD_NOREPEAT, VK_F10);

    MSG msg{};
    bool run = true;
    while (run)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) { run = false; break; }
            if (msg.message == WM_HOTKEY)
            {
                switch (msg.wParam)
                {
                case HK_TOGGLE: if (g_active) StopScaling(); else StartScaling(); break;
                case HK_SHARP_DN: g_sharp = std::max(0.0f, g_sharp - 0.05f); break;
                case HK_SHARP_UP: g_sharp = std::min(1.0f, g_sharp + 0.05f); break;
                case HK_SAT_DN: g_sat = std::max(0.5f, g_sat - 0.02f); break;
                case HK_SAT_UP: g_sat = std::min(1.6f, g_sat + 0.02f); break;
                case HK_OLED: g_oled = !g_oled; break;
                case HK_ENGINE: g_engine = !g_engine; break;
                case HK_QUIT: run = false; break;
                }
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!run) break;

        if (g_active) RenderFrame();
        else WaitMessage();
    }

    SaveCfg();
    for (int id = HK_TOGGLE; id <= HK_QUIT; ++id) UnregisterHotKey(nullptr, id);
    if (g_hwnd) DestroyWindow(g_hwnd);
    return 0;
}
