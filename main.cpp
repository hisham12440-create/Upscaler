// =====================================================================
//  AH Upscaler - Phase 5 (Detail Engine + OLED Shadow Detail, no frame gen)
//  Build (x64 Native Tools Command Prompt):
//  cl /O2 /EHsc /std:c++17 /utf-8 /DUNICODE /D_UNICODE main.cpp /link /SUBSYSTEM:WINDOWS
// =====================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <algorithm>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winmm.lib")

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

using Microsoft::WRL::ComPtr;

// =====================================================================
// 1) Logging
// =====================================================================
static FILE* g_log = nullptr;

static void LogInit(const std::string& path) { fopen_s(&g_log, path.c_str(), "w"); }

static void Log(const char* fmt, ...) {
    if (!g_log) return;
    va_list a; va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}

[[noreturn]] static void Fatal(const char* msg, HRESULT hr = 0) {
    char buf[4096];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s (HRESULT 0x%08X)", msg, (unsigned)hr);
    Log("FATAL: %s", buf);
    MessageBoxA(nullptr, buf, "AH Upscaler", MB_OK | MB_ICONERROR);
    ExitProcess(1);
}

#define AH_CHECK(expr, msg) do { HRESULT _hr = (expr); if (FAILED(_hr)) Fatal(msg, _hr); } while (0)

// =====================================================================
// 2) Config
// =====================================================================
struct Config {
    int   monitor = 0;
    int   vsync = 1;            // -1 auto, 0 off, 1 on
    bool  oled = true;
    bool  layered = true;       // set 0 if you get a black screen
    float sharpness = 0.70f;    // luma-only adaptive sharpening
    float clarity = 0.40f;      // multi-scale local contrast
    float gate = 0.50f;         // noise gate: higher = less sharpening in flat areas
    float overshoot = 0.03f;    // halo limiter: lower = fewer halos
    float denoise = 0.25f;      // edge-preserving denoise before sharpening
    float shadowDetail = 0.35f; // lifts dark detail but keeps true black
    float saturation = 1.15f;
    float blackLevel = 0.015f;
    float depth = 0.25f;        // OLED shadow depth (0 = off)
    float gamma = 1.08f;
    float contrast = 0.30f;
};

static std::string ExeDir() {
    char p[MAX_PATH];
    GetModuleFileNameA(nullptr, p, MAX_PATH);
    std::string s(p);
    return s.substr(0, s.find_last_of("\\/") + 1);
}

static float Clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

struct IniDefault { const char* key; const char* val; };
static const IniDefault kDefaults[] = {
    {"monitor", "0"}, {"vsync", "1"}, {"oled", "1"}, {"layered", "1"},
    {"sharpness", "0.70"}, {"clarity", "0.40"}, {"gate", "0.50"}, {"overshoot", "0.03"},
    {"denoise", "0.25"}, {"shadow_detail", "0.35"},
    {"saturation", "1.15"}, {"black_level", "0.015"}, {"black_depth", "0.25"},
    {"gamma", "1.08"}, {"contrast", "0.30"}
};

static std::string IniGet(const std::string& file, const char* key, const char* def) {
    char b[64];
    GetPrivateProfileStringA("main", key, def, b, sizeof(b), file.c_str());
    return b;
}

static void LoadConfig(Config& c, const std::string& file) {
    if (GetFileAttributesA(file.c_str()) == INVALID_FILE_ATTRIBUTES) {
        for (auto& d : kDefaults) WritePrivateProfileStringA("main", d.key, d.val, file.c_str());
        Log("config.ini created with defaults");
    }
    auto I = [&](const char* k, const char* d) { return atoi(IniGet(file, k, d).c_str()); };
    auto F = [&](const char* k, const char* d) { return (float)atof(IniGet(file, k, d).c_str()); };
    c.monitor      = std::max(0, I("monitor", "0"));
    c.vsync        = I("vsync", "1");
    c.oled         = I("oled", "1") != 0;
    c.layered      = I("layered", "1") != 0;
    c.sharpness    = Clampf(F("sharpness", "0.70"), 0.0f, 1.0f);
    c.clarity      = Clampf(F("clarity", "0.40"), 0.0f, 1.0f);
    c.gate         = Clampf(F("gate", "0.50"), 0.0f, 1.0f);
    c.overshoot    = Clampf(F("overshoot", "0.03"), 0.0f, 0.2f);
    c.denoise      = Clampf(F("denoise", "0.25"), 0.0f, 1.0f);
    c.shadowDetail = Clampf(F("shadow_detail", "0.35"), 0.0f, 1.0f);
    c.saturation   = Clampf(F("saturation", "1.15"), 0.0f, 2.0f);
    c.blackLevel   = Clampf(F("black_level", "0.015"), 0.0f, 0.2f);
    c.depth        = Clampf(F("black_depth", "0.25"), 0.0f, 0.9f);
    c.gamma        = Clampf(F("gamma", "1.08"), 0.5f, 2.0f);
    c.contrast     = Clampf(F("contrast", "0.30"), 0.0f, 1.0f);
    Log("config: monitor=%d vsync=%d sharp=%.2f clarity=%.2f gate=%.2f overshoot=%.3f denoise=%.2f shadow=%.2f oled=%d",
        c.monitor, c.vsync, c.sharpness, c.clarity, c.gate, c.overshoot, c.denoise, c.shadowDetail, (int)c.oled);
}

// =====================================================================
// 3) Shader (HLSL) - single fused detail pass
// =====================================================================
static const char* kShaderSrc = R"HLSL(
cbuffer C : register(b0) {
    float2 srcSize; float2 dstSize;
    float sharp; float clarity; float sat; float black;
    float gam; float contrast; float oled; float depth;
    float4 extra;   // x = noise gate, y = overshoot limit, z = effects on (1) / bypass (0), w = strength
    float4 extra2;  // x = denoise, y = shadow detail
};
Texture2D tex : register(t0);

struct VSOut { float4 p : SV_Position; float2 uv : TEXCOORD0; };

VSOut VS(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

float3 L(int2 p) {
    int2 q = clamp(p, int2(0, 0), int2(srcSize) - int2(1, 1));
    return tex.Load(int3(q, 0)).rgb;
}

float IGN(float2 p) { return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715)))); }

static const float kInv = 1.0 / (2.0 * 0.07 * 0.07);   // denoise range sigma = 0.07

float4 PSFinal(VSOut i) : SV_Target {
    int2 p = int2(floor(i.uv * srcSize));
    float3 e = L(p);
    if (extra.z < 0.5) return float4(e, 1);

    const float str = extra.w;

    // ---- 3x3 neighbourhood ----
    float3 N[9];
    float  Y[9];
    [unroll] for (int k = 0; k < 9; k++) {
        int2 o = int2(k % 3 - 1, k / 3 - 1);
        N[k] = L(p + o);
        Y[k] = Luma(N[k]);
    }

    // ---- edge-preserving denoise (bilateral on luma) ----
    float3 acc = N[4];
    float  ws = 1.0;
    [unroll] for (int k2 = 0; k2 < 9; k2++) {
        if (k2 == 4) continue;
        float d = Y[k2] - Y[4];
        float w = exp(-d * d * kInv);
        acc += N[k2] * w;
        ws += w;
    }
    float3 base = lerp(N[4], acc / ws, saturate(extra2.x));
    float lbase = Luma(base);

    // ---- local statistics ----
    float lmn = Y[0], lmx = Y[0];
    [unroll] for (int k3 = 1; k3 < 9; k3++) { lmn = min(lmn, Y[k3]); lmx = max(lmx, Y[k3]); }
    float blur = (Y[0] + Y[2] + Y[6] + Y[8] + 2.0 * (Y[1] + Y[3] + Y[5] + Y[7]) + 4.0 * Y[4]) * (1.0 / 16.0);

    // ---- adaptive luma sharpening (CAS-like headroom) + noise gate ----
    float amp = sqrt(saturate(min(lmn, 1.0 - lmx) / max(lmx, 1e-4)));
    float rng = lmx - lmn;
    float g0 = 0.012 * extra.x;
    float gate = smoothstep(g0, g0 + 0.05, rng);
    float dl = (lbase - blur) * (sharp * str * 3.0) * amp * gate;

    // ---- medium-scale clarity (ring at distance 2), midtone-weighted ----
    if (clarity > 0.001) {
        float yr = 0.0;
        [unroll] for (int k4 = 0; k4 < 9; k4++) {
            if (k4 == 4) continue;
            int2 o2 = int2((k4 % 3 - 1) * 2, (k4 / 3 - 1) * 2);
            yr += Luma(L(p + o2));
        }
        yr *= 0.125;
        float broad = lerp(blur, yr, 0.6);
        float dtl = clamp((lbase - broad) * (clarity * str * 1.4), -0.07, 0.07);
        float t = abs(2.0 * lbase - 1.0);
        float mask = saturate(1.0 - t * t + 0.15);
        dl += dtl * mask * lerp(0.5, 1.0, gate);
    }

    // ---- halo limiter ----
    float lnew = clamp(lbase + dl, lmn - extra.y, lmx + extra.y);
    float3 c = saturate(base + (lnew - lbase));

    // ---- OLED look ----
    if (oled > 0.5) {
        // vibrance: boost muted colors more than already-saturated ones
        float l = Luma(c);
        float chroma = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
        float vib = 1.0 + (sat - 1.0) * (1.0 - saturate(chroma));
        c = saturate(lerp(l.xxx, c, vib));
        // true-black floor
        c = saturate((c - black) / (1.0 - black));
        // shadow falloff: darker darks, hue preserved
        float l2 = Luma(c);
        c *= lerp(1.0 - depth, 1.0, smoothstep(0.0, 0.35, l2));
        // shadow detail: lift dark-but-not-black pixels, true black stays untouched
        float l3 = Luma(c);
        float lift = 1.0 + extra2.y * 0.35 * smoothstep(0.004, 0.03, l3) * (1.0 - smoothstep(0.03, 0.22, l3));
        c = saturate(c * lift);
        c = pow(c, gam);
        c = lerp(c, c * c * (3.0 - 2.0 * c), contrast);
    }

    // ---- triangular dither against banding ----
    float2 q = i.p.xy;
    c += (IGN(q) + IGN(q + 5.588238) - 1.0) / 255.0;
    return float4(saturate(c), 1);
}
)HLSL";

// =====================================================================
// 4) Screen capture (Desktop Duplication)
// =====================================================================
enum class CapResult { NewFrame, NoChange, Lost };

class Capture {
public:
    bool Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, IDXGIOutput* output) {
        dev_ = dev; ctx_ = ctx;
        ComPtr<IDXGIOutput> o(output);
        AH_CHECK(o.As(&out_), "IDXGIOutput1 not available");
        return Open();
    }
    bool Recreate() { return Open(); }

    // Waits up to timeoutMs for a new desktop frame
    CapResult Acquire(UINT timeoutMs) {
        if (!dup_) return CapResult::Lost;
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> res;
        HRESULT hr = dup_->AcquireNextFrame(timeoutMs, &info, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return CapResult::NoChange;
        if (FAILED(hr)) { dup_.Reset(); return CapResult::Lost; }
        bool got = false;
        if (info.LastPresentTime.QuadPart != 0) {
            ComPtr<ID3D11Texture2D> t;
            if (SUCCEEDED(res.As(&t))) { ctx_->CopyResource(tex_.Get(), t.Get()); got = true; }
        }
        dup_->ReleaseFrame();
        return got ? CapResult::NewFrame : CapResult::NoChange;
    }

    ID3D11Texture2D* Tex() const { return tex_.Get(); }
    int Width() const { return w_; }
    int Height() const { return h_; }

private:
    bool Open() {
        dup_.Reset();
        HRESULT hr = out_->DuplicateOutput(dev_.Get(), &dup_);
        if (FAILED(hr)) { Log("DuplicateOutput failed 0x%08X", (unsigned)hr); return false; }
        DXGI_OUTDUPL_DESC dd{};
        dup_->GetDesc(&dd);
        if (!tex_ || w_ != (int)dd.ModeDesc.Width || h_ != (int)dd.ModeDesc.Height || fmt_ != dd.ModeDesc.Format) {
            w_ = (int)dd.ModeDesc.Width; h_ = (int)dd.ModeDesc.Height; fmt_ = dd.ModeDesc.Format;
            if (fmt_ != DXGI_FORMAT_B8G8R8A8_UNORM) Log("WARNING: capture format %d (HDR on?)", (int)fmt_);
            D3D11_TEXTURE2D_DESC td{};
            td.Width = w_; td.Height = h_; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = fmt_; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            tex_.Reset();
            AH_CHECK(dev_->CreateTexture2D(&td, nullptr, &tex_), "Create capture texture");
        }
        Log("capture opened %dx%d", w_, h_);
        return true;
    }

    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGIOutput1> out_;
    ComPtr<IDXGIOutputDuplication> dup_;
    ComPtr<ID3D11Texture2D> tex_;
    int w_ = 0, h_ = 0;
    DXGI_FORMAT fmt_ = DXGI_FORMAT_UNKNOWN;
};

// =====================================================================
// 5) Renderer
// =====================================================================
struct alignas(16) CBData {
    float srcSize[2]; float dstSize[2];
    float sharp, clarity, sat, black;
    float gam, contrast, oled, depth;
    float extra[4];
    float extra2[4];
};

class Renderer {
public:
    void Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, IDXGIFactory2* fac, HWND hwnd, int dw, int dh) {
        dev_ = dev; ctx_ = ctx; dispW_ = dw; dispH_ = dh;

        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = dw; sd.Height = dh; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        AH_CHECK(fac->CreateSwapChainForHwnd(dev, hwnd, &sd, nullptr, nullptr, &sc_), "CreateSwapChain");
        fac->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        ComPtr<ID3D11Texture2D> bb;
        AH_CHECK(sc_->GetBuffer(0, IID_PPV_ARGS(&bb)), "GetBuffer");
        AH_CHECK(dev->CreateRenderTargetView(bb.Get(), nullptr, &bbRTV_), "Backbuffer RTV");

        auto vsB = Compile("VS", "vs_5_0");
        AH_CHECK(dev->CreateVertexShader(vsB->GetBufferPointer(), vsB->GetBufferSize(), nullptr, &vs_), "VS");
        auto psB = Compile("PSFinal", "ps_5_0");
        AH_CHECK(dev->CreatePixelShader(psB->GetBufferPointer(), psB->GetBufferSize(), nullptr, &ps_), "PS");

        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(CBData); bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        AH_CHECK(dev->CreateBuffer(&bd, nullptr, &cb_), "Constant buffer");

        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs_.Get(), nullptr, 0);
        ctx->PSSetShader(ps_.Get(), nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
    }

    // Call after Init and after every successful capture Recreate()
    void SetSource(ID3D11Texture2D* t, int w, int h) {
        srcW_ = w; srcH_ = h;
        srcSRV_.Reset();
        AH_CHECK(dev_->CreateShaderResourceView(t, nullptr, &srcSRV_), "Source SRV");
        Log("source %dx%d", w, h);
    }

    void Render(const Config& cfg, bool oledOn, bool fxOn, float strength) {
        if (!srcSRV_) return;
        CBData cb{};
        cb.srcSize[0] = (float)srcW_;  cb.srcSize[1] = (float)srcH_;
        cb.dstSize[0] = (float)dispW_; cb.dstSize[1] = (float)dispH_;
        cb.sharp = cfg.sharpness; cb.clarity = cfg.clarity;
        cb.sat = cfg.saturation;  cb.black = cfg.blackLevel;
        cb.gam = cfg.gamma;       cb.contrast = cfg.contrast;
        cb.oled = oledOn ? 1.0f : 0.0f;
        cb.depth = cfg.depth;
        cb.extra[0] = cfg.gate; cb.extra[1] = cfg.overshoot;
        cb.extra[2] = fxOn ? 1.0f : 0.0f; cb.extra[3] = strength;
        cb.extra2[0] = cfg.denoise; cb.extra2[1] = cfg.shadowDetail;

        ctx_->UpdateSubresource(cb_.Get(), 0, nullptr, &cb, 0, 0);
        D3D11_VIEWPORT vp{0.0f, 0.0f, (float)dispW_, (float)dispH_, 0.0f, 1.0f};
        ctx_->RSSetViewports(1, &vp);
        ctx_->OMSetRenderTargets(1, bbRTV_.GetAddressOf(), nullptr);
        ID3D11ShaderResourceView* s[1] = { srcSRV_.Get() };
        ctx_->PSSetShaderResources(0, 1, s);
        ctx_->Draw(3, 0);
        ID3D11ShaderResourceView* none[1] = { nullptr };
        ctx_->PSSetShaderResources(0, 1, none);
    }

    void Present(bool vsync) { sc_->Present(vsync ? 1 : 0, 0); }

private:
    static ComPtr<ID3DBlob> Compile(const char* entry, const char* target) {
        ComPtr<ID3DBlob> code, err;
        HRESULT hr = D3DCompile(kShaderSrc, strlen(kShaderSrc), "ah.hlsl", nullptr, nullptr,
                                entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
        if (FAILED(hr)) {
            std::string m = std::string("Shader compile failed: ") + entry + "\n" +
                            (err ? (const char*)err->GetBufferPointer() : "unknown");
            Fatal(m.c_str(), hr);
        }
        return code;
    }

    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain1> sc_;
    ComPtr<ID3D11RenderTargetView> bbRTV_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> cb_;
    ComPtr<ID3D11ShaderResourceView> srcSRV_;
    int dispW_ = 0, dispH_ = 0, srcW_ = 0, srcH_ = 0;
};

// =====================================================================
// 6) Main
// =====================================================================
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"AH_Upscaler_Single_Instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    SetProcessDPIAware();
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    const std::string dir = ExeDir();
    LogInit(dir + "upscaler.log");
    Log("AH Upscaler Phase 5 starting");

    Config cfg;
    const std::string iniPath = dir + "config.ini";
    LoadConfig(cfg, iniPath);

    ComPtr<IDXGIFactory1> f1;
    AH_CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&f1)), "CreateDXGIFactory1");
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    for (UINT a = 0; ; ++a) {
        ComPtr<IDXGIAdapter1> ad;
        if (f1->EnumAdapters1(a, &ad) == DXGI_ERROR_NOT_FOUND) break;
        ComPtr<IDXGIOutput> o;
        if (SUCCEEDED(ad->EnumOutputs((UINT)cfg.monitor, &o))) { adapter = ad; output = o; break; }
    }
    if (!output) Fatal("Monitor not found. Check 'monitor' in config.ini");

    DXGI_OUTPUT_DESC od{};
    output->GetDesc(&od);
    const RECT rc = od.DesktopCoordinates;
    const int W = rc.right - rc.left, H = rc.bottom - rc.top;
    Log("monitor %d: %dx%d at (%ld,%ld)", cfg.monitor, W, H, rc.left, rc.top);

    DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    int hz = 60;
    if (EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) hz = (int)dm.dmDisplayFrequency;
    const bool vsync = cfg.vsync < 0 ? (hz <= 75) : (cfg.vsync != 0);
    Log("display %d Hz, vsync=%d", hz, (int)vsync);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.lpszClassName = L"AHUpscalerWnd";
    RegisterClassW(&wc);
    DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
    if (cfg.layered) ex |= WS_EX_LAYERED;
    HWND hwnd = CreateWindowExW(ex, wc.lpszClassName, L"AH Upscaler", WS_POPUP,
                                rc.left, rc.top, W, H, nullptr, nullptr, hi, nullptr);
    if (!hwnd) Fatal("CreateWindow failed", HRESULT_FROM_WIN32(GetLastError()));
    if (cfg.layered) SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    if (!SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE))
        Log("WARNING: WDA_EXCLUDEFROMCAPTURE unsupported (need Windows 10 2004+). Feedback loop likely.");
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    RegisterHotKey(hwnd, 1, MOD_NOREPEAT, VK_F8);   // reload config
    RegisterHotKey(hwnd, 2, MOD_NOREPEAT, VK_F9);   // toggle OLED look
    RegisterHotKey(hwnd, 3, MOD_NOREPEAT, VK_F10);  // exit
    RegisterHotKey(hwnd, 4, MOD_NOREPEAT, VK_F11);  // pause / resume
    RegisterHotKey(hwnd, 5, MOD_NOREPEAT, VK_F7);   // A/B: all enhancements on/off
    RegisterHotKey(hwnd, 6, MOD_NOREPEAT, VK_F6);   // cycle enhancement strength

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    AH_CHECK(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, &dev, nullptr, &ctx), "D3D11CreateDevice");

    // Keep at most one frame queued on the GPU -> lower input latency
    {
        ComPtr<IDXGIDevice1> dxgiDev;
        if (SUCCEEDED(dev.As(&dxgiDev))) dxgiDev->SetMaximumFrameLatency(1);
    }

    ComPtr<IDXGIFactory2> f2;
    AH_CHECK(f1.As(&f2), "IDXGIFactory2 not available");

    Capture capture;
    if (!capture.Init(dev.Get(), ctx.Get(), output.Get()))
        Fatal("Desktop capture failed. Close other capture apps, or the monitor is on another GPU.");

    Renderer renderer;
    renderer.Init(dev.Get(), ctx.Get(), f2.Get(), hwnd, W, H);
    renderer.SetSource(capture.Tex(), capture.Width(), capture.Height());

    timeBeginPeriod(1);

    const float kStrength[3] = { 0.6f, 1.0f, 1.5f };
    int sIdx = 1;

    bool running = true, paused = false, oledOn = cfg.oled, fxOn = true, dirty = false;
    unsigned long long statStart = GetTickCount64();
    int presented = 0, captured = 0;

    while (running) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) { running = false; break; }
            if (m.message == WM_HOTKEY) {
                switch (m.wParam) {
                case 1: LoadConfig(cfg, iniPath); oledOn = cfg.oled; dirty = true; break;
                case 2: oledOn = !oledOn; dirty = true; break;
                case 3: running = false; break;
                case 4:
                    paused = !paused;
                    ShowWindow(hwnd, paused ? SW_HIDE : SW_SHOWNOACTIVATE);
                    if (!paused) dirty = true;
                    break;
                case 5: fxOn = !fxOn; dirty = true; Log("enhancements %s", fxOn ? "ON" : "OFF"); break;
                case 6: sIdx = (sIdx + 1) % 3; dirty = true; Log("strength %.1f", kStrength[sIdx]); break;
                }
            }
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (!running) break;
        if (paused) { Sleep(50); continue; }

        CapResult r = capture.Acquire(8);
        if (r == CapResult::Lost) {
            Sleep(100);
            if (capture.Recreate())
                renderer.SetSource(capture.Tex(), capture.Width(), capture.Height());
            dirty = true;
            continue;
        }

        const bool newFrame = (r == CapResult::NewFrame);
        if (newFrame) ++captured;

        if (newFrame || dirty) {
            renderer.Render(cfg, oledOn, fxOn, kStrength[sIdx]);
            renderer.Present(vsync);
            ++presented;
            dirty = false;
        }

        const unsigned long long now = GetTickCount64();
        if (now - statStart >= 5000) {
            double s = (double)(now - statStart) / 1000.0;
            Log("present %.1f fps | source %.1f fps", presented / s, captured / s);
            presented = captured = 0;
            statStart = now;
        }
    }

    timeEndPeriod(1);
    Log("exit");
    if (g_log) fclose(g_log);
    if (mutex) CloseHandle(mutex);
    return 0;
}
