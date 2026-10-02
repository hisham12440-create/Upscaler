// =====================================================================
//  AH Upscaler - Phase 4 "Smooth"
//  Build (x64 Native Tools Command Prompt):
//  cl /O2 /EHsc /std:c++17 /utf-8 /DUNICODE /D_UNICODE main.cpp /link /SUBSYSTEM:WINDOWS
// =====================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <d3d11.h>
#include <dxgi1_3.h>
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

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#ifndef CREATE_WAITABLE_TIMER_MANUAL_RESET
#define CREATE_WAITABLE_TIMER_MANUAL_RESET 0x00000001
#endif
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
// 2) Config (config.ini is reset automatically when its version changes)
// =====================================================================
static const int kCfgVersion = 4;

struct Config {
    int   monitor = 0;
    int   targetFps = 60;
    bool  oled = true;
    bool  layered = true;
    bool  adaptive = true;
    bool  framegen = true;
    float scale = 1.3333f;     // 1.3333 = 2.5K on a 1080p screen
    float minScale = 1.0f;
    int   fgSearch = 16;
    float sharpness = 0.5f;
    float clarity = 0.2f;
    float saturation = 1.2f;   // vibrance strength
    float blackLevel = 0.015f;
    float gamma = 1.06f;
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
    {"version", "4"}, {"monitor", "0"}, {"target_fps", "60"}, {"oled", "1"}, {"layered", "1"},
    {"scale", "1.3333"}, {"min_scale", "1.0"}, {"adaptive", "1"}, {"framegen", "1"}, {"fg_search", "16"},
    {"sharpness", "0.5"}, {"clarity", "0.2"}, {"saturation", "1.2"},
    {"black_level", "0.015"}, {"gamma", "1.06"}, {"contrast", "0.30"}
};

static std::string IniGet(const std::string& file, const char* key, const char* def) {
    char b[64];
    GetPrivateProfileStringA("main", key, def, b, sizeof(b), file.c_str());
    return b;
}

static void LoadConfig(Config& c, const std::string& file) {
    const bool fresh = GetFileAttributesA(file.c_str()) == INVALID_FILE_ATTRIBUTES;
    if (fresh || atoi(IniGet(file, "version", "0").c_str()) != kCfgVersion) {
        for (auto& d : kDefaults) WritePrivateProfileStringA("main", d.key, d.val, file.c_str());
        Log("config.ini written with Phase 4 defaults");
    }
    auto I = [&](const char* k, const char* d) { return atoi(IniGet(file, k, d).c_str()); };
    auto F = [&](const char* k, const char* d) { return (float)atof(IniGet(file, k, d).c_str()); };
    c.monitor    = std::max(0, I("monitor", "0"));
    c.targetFps  = std::clamp(I("target_fps", "60"), 24, 240);
    c.oled       = I("oled", "1") != 0;
    c.layered    = I("layered", "1") != 0;
    c.scale      = Clampf(F("scale", "1.3333"), 1.0f, 2.0f);
    c.minScale   = Clampf(F("min_scale", "1.0"), 1.0f, c.scale);
    c.adaptive   = I("adaptive", "1") != 0;
    c.framegen   = I("framegen", "1") != 0;
    c.fgSearch   = std::clamp(I("fg_search", "16"), 4, 32) & ~1;
    c.sharpness  = Clampf(F("sharpness", "0.5"), 0.0f, 1.0f);
    c.clarity    = Clampf(F("clarity", "0.2"), 0.0f, 1.0f);
    c.saturation = Clampf(F("saturation", "1.2"), 0.5f, 2.0f);
    c.blackLevel = Clampf(F("black_level", "0.015"), 0.0f, 0.2f);
    c.gamma      = Clampf(F("gamma", "1.06"), 0.5f, 2.0f);
    c.contrast   = Clampf(F("contrast", "0.30"), 0.0f, 1.0f);
    Log("config: monitor=%d fps=%d scale=%.2f..%.2f adaptive=%d framegen=%d search=%d oled=%d",
        c.monitor, c.targetFps, c.minScale, c.scale, (int)c.adaptive, (int)c.framegen, c.fgSearch, (int)c.oled);
}

// =====================================================================
// 3) Shaders (HLSL)
// =====================================================================
static const char* kShaderSrc = R"HLSL(
cbuffer C : register(b0) {
    float2 srcSize; float2 dstSize;
    float sharp; float clarity; float vib; float black;
    float gam; float contrast; float oled; float frame;
    float4 P0;   // x = interpolation phase, y = search range (px), z = SAD threshold
    float4 P1;   // xy = 1 / motion grid size
};
Texture2D gA : register(t0);
Texture2D gB : register(t1);
Texture2D gM : register(t2);
Texture2D gC : register(t3);
SamplerState gS : register(s0);

struct VSOut { float4 p : SV_Position; float2 uv : TEXCOORD0; };

VSOut VS(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// ---------------- Hierarchical motion estimation ----------------
// gA = previous frame, gB = current frame.
// Result = displacement from a current-frame block to its match in the previous frame.
float BlockSAD(float2 base, float2 d, float sp, float cl[16]) {
    float s = 0;
    [unroll] for (int j = 0; j < 4; j++) {
        [unroll] for (int k = 0; k < 4; k++) {
            float2 pp = base + d + (float2(k, j) + 0.5) * sp;
            float l = Luma(gA.SampleLevel(gS, pp / srcSize, 0).rgb);
            s += abs(l - cl[j * 4 + k]);
        }
    }
    return s * 0.0625;
}

float4 PSMotionCoarse(VSOut i) : SV_Target {
    float2 base = floor(i.p.xy) * 16.0;
    float cl[16];
    [unroll] for (int j = 0; j < 4; j++) {
        [unroll] for (int k = 0; k < 4; k++) {
            float2 pp = base + (float2(k, j) + 0.5) * 4.0;
            cl[j * 4 + k] = Luma(gB.SampleLevel(gS, pp / srcSize, 0).rgb);
        }
    }
    float best = BlockSAD(base, float2(0, 0), 4.0, cl) - 0.004;
    float2 bd = float2(0, 0);
    int n = (int)(P0.y * 0.5);
    [loop] for (int y = -n; y <= n; y++) {
        [loop] for (int x = -n; x <= n; x++) {
            float2 d = float2(x, y) * 4.0;
            float s = BlockSAD(base, d, 4.0, cl);
            if (s < best) { best = s; bd = d; }
        }
    }
    return float4(bd, max(best, 0.0), 1);
}

float4 PSMotionFine(VSOut i) : SV_Target {
    int2 fb = int2(floor(i.p.xy));
    float2 base = float2(fb) * 8.0;
    float cl[16];
    [unroll] for (int j = 0; j < 4; j++) {
        [unroll] for (int k = 0; k < 4; k++) {
            float2 pp = base + (float2(k, j) + 0.5) * 2.0;
            cl[j * 4 + k] = Luma(gB.SampleLevel(gS, pp / srcSize, 0).rgb);
        }
    }
    float2 cv = gC.Load(int3(fb / 2, 0)).xy;
    float best = BlockSAD(base, cv, 2.0, cl);
    float2 bd = cv;
    float sz = BlockSAD(base, float2(0, 0), 2.0, cl);
    if (sz - 0.004 < best) { best = sz; bd = float2(0, 0); }

    float2 c0 = bd;
    [loop] for (int y = -2; y <= 2; y++) {
        [loop] for (int x = -2; x <= 2; x++) {
            float2 d = c0 + float2(x, y) * 2.0;
            float s = BlockSAD(base, d, 2.0, cl);
            if (s < best) { best = s; bd = d; }
        }
    }
    c0 = bd;
    [loop] for (int yy = -1; yy <= 1; yy++) {
        [loop] for (int xx = -1; xx <= 1; xx++) {
            float2 d = c0 + float2(xx, yy);
            float s = BlockSAD(base, d, 2.0, cl);
            if (s < best) { best = s; bd = d; }
        }
    }
    return float4(bd / srcSize, best, 1);
}

// ---------------- Motion-compensated interpolation at phase t ----------------
float4 PSInterp(VSOut i) : SV_Target {
    float t = P0.x;
    float4 m = gM.SampleLevel(gS, i.uv, 0);
    float2 d = m.xy;
    float conf = 1.0 - smoothstep(P0.z * 0.5, P0.z, m.z);

    float2 g = P1.xy;
    float2 a1 = gM.SampleLevel(gS, i.uv + float2( g.x, 0), 0).xy;
    float2 a2 = gM.SampleLevel(gS, i.uv + float2(-g.x, 0), 0).xy;
    float2 a3 = gM.SampleLevel(gS, i.uv + float2(0,  g.y), 0).xy;
    float2 a4 = gM.SampleLevel(gS, i.uv + float2(0, -g.y), 0).xy;
    float disc = max(max(length((a1 - d) * srcSize), length((a2 - d) * srcSize)),
                     max(length((a3 - d) * srcSize), length((a4 - d) * srcSize)));
    conf *= 1.0 - smoothstep(2.0, 8.0, disc);

    float3 p0 = gA.SampleLevel(gS, i.uv + d * t, 0).rgb;
    float3 c0 = gB.SampleLevel(gS, i.uv - d * (1.0 - t), 0).rgb;
    float3 warped = lerp(p0, c0, t);
    // Fallback is the nearest REAL frame (no blending = no double image)
    float3 pa = gA.SampleLevel(gS, i.uv, 0).rgb;
    float3 pb = gB.SampleLevel(gS, i.uv, 0).rgb;
    float3 plain = (t < 0.5) ? pa : pb;
    float diff = length(p0 - c0);
    float ok = conf * (1.0 - smoothstep(0.12, 0.35, diff));
    ok = smoothstep(0.3, 0.7, ok);
    return float4(lerp(plain, warped, ok), 1);
}

// ---------------- Resampling ----------------
float3 CatmullRom(float2 uv) {
    float2 pos = uv * srcSize;
    float2 p1 = floor(pos - 0.5) + 0.5;
    float2 f = pos - p1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 off = w2 / w12;
    float2 t0 = (p1 - 1.0) / srcSize;
    float2 t3 = (p1 + 2.0) / srcSize;
    float2 t12 = (p1 + off) / srcSize;
    float3 r = 0;
    r += gA.SampleLevel(gS, float2(t0.x,  t0.y),  0).rgb * (w0.x  * w0.y);
    r += gA.SampleLevel(gS, float2(t12.x, t0.y),  0).rgb * (w12.x * w0.y);
    r += gA.SampleLevel(gS, float2(t3.x,  t0.y),  0).rgb * (w3.x  * w0.y);
    r += gA.SampleLevel(gS, float2(t0.x,  t12.y), 0).rgb * (w0.x  * w12.y);
    r += gA.SampleLevel(gS, float2(t12.x, t12.y), 0).rgb * (w12.x * w12.y);
    r += gA.SampleLevel(gS, float2(t3.x,  t12.y), 0).rgb * (w3.x  * w12.y);
    r += gA.SampleLevel(gS, float2(t0.x,  t3.y),  0).rgb * (w0.x  * w3.y);
    r += gA.SampleLevel(gS, float2(t12.x, t3.y),  0).rgb * (w12.x * w3.y);
    r += gA.SampleLevel(gS, float2(t3.x,  t3.y),  0).rgb * (w3.x  * w3.y);
    return r;
}

float4 PSUpscale(VSOut i) : SV_Target {
    return float4(saturate(CatmullRom(i.uv)), 1);
}

// ---------------- Final: resample + sharpen + clarity + OLED look ----------------
float4 PSFinal(VSOut i) : SV_Target {
    float2 t = 1.0 / dstSize;
    float3 c;
    if (abs(srcSize.x - dstSize.x) < 0.5) c = gA.SampleLevel(gS, i.uv, 0).rgb;
    else c = CatmullRom(i.uv);

    float3 n = gA.SampleLevel(gS, i.uv + float2(0, -t.y), 0).rgb;
    float3 s = gA.SampleLevel(gS, i.uv + float2(0,  t.y), 0).rgb;
    float3 w = gA.SampleLevel(gS, i.uv + float2(-t.x, 0), 0).rgb;
    float3 e = gA.SampleLevel(gS, i.uv + float2( t.x, 0), 0).rgb;

    if (sharp > 0.001) {
        float3 mn = min(c, min(min(n, s), min(w, e)));
        float3 mx = max(c, max(max(n, s), max(w, e)));
        float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));
        float peak = -1.0 / lerp(8.0, 5.0, sharp);
        float3 k = amp * peak;
        c = saturate((k * (n + s + w + e) + c) / (1.0 + 4.0 * k));
    }

    if (clarity > 0.001) {
        float2 r = 3.0 * t;
        float3 avg = 0.25 * (
            gA.SampleLevel(gS, i.uv + float2( r.x, 0), 0).rgb +
            gA.SampleLevel(gS, i.uv + float2(-r.x, 0), 0).rgb +
            gA.SampleLevel(gS, i.uv + float2(0,  r.y), 0).rgb +
            gA.SampleLevel(gS, i.uv + float2(0, -r.y), 0).rgb);
        c = saturate(c + (c - avg) * clarity * 0.8);
    }

    if (oled > 0.5) {
        // tone curve on luminance only, so hues are preserved
        float l = Luma(c);
        float lo = saturate((l - black) / (1.0 - black));
        lo = pow(lo, gam);
        lo = lerp(lo, lo * lo * (3.0 - 2.0 * lo), contrast);
        float ratio = min(lo / max(l, 1e-4), 4.0);
        c *= ratio;
        // vibrance: boosts dull colours more than already-saturated ones
        float mxc = max(c.r, max(c.g, c.b));
        float mnc = min(c.r, min(c.g, c.b));
        float lc = Luma(c);
        c = lerp(lc.xxx, c, 1.0 + (vib - 1.0) * (1.0 - (mxc - mnc)));
        float m2 = max(c.r, max(c.g, c.b));
        if (m2 > 1.0) c /= m2;
    }

    // interleaved gradient noise dithering (kills banding in dark gradients)
    float nz = frac(52.9829189 * frac(dot(i.p.xy + frame * 5.588238, float2(0.06711056, 0.00583715))));
    c += (nz - 0.5) / 255.0;
    return float4(saturate(c), 1);
}
)HLSL";

// =====================================================================
// 4) Frame pacer (used only when the display rate is not a multiple of the target)
// =====================================================================
class FramePacer {
public:
    ~FramePacer() { if (timer_) CloseHandle(timer_); }
    void Init(int fps) {
        QueryPerformanceFrequency(&freq_);
        ticks_ = (double)freq_.QuadPart / (double)fps;
        if (!timer_) {
            timer_ = CreateWaitableTimerExW(nullptr, nullptr,
                        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION | CREATE_WAITABLE_TIMER_MANUAL_RESET,
                        TIMER_ALL_ACCESS);
            if (!timer_) timer_ = CreateWaitableTimerW(nullptr, TRUE, nullptr);
        }
        Reset();
    }
    void Reset() { LARGE_INTEGER n; QueryPerformanceCounter(&n); next_ = (double)n.QuadPart; }
    void Wait() {
        next_ += ticks_;
        LARGE_INTEGER n; QueryPerformanceCounter(&n);
        double left = next_ - (double)n.QuadPart;
        double f = (double)freq_.QuadPart;
        if (timer_ && left > f * 0.003) {
            double sec = (left - f * 0.002) / f;
            LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(sec * 1.0e7);
            SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer_, 100);
        }
        for (;;) {
            QueryPerformanceCounter(&n);
            if ((double)n.QuadPart >= next_) break;
            YieldProcessor();
        }
        if ((double)n.QuadPart > next_ + ticks_ * 2.0) next_ = (double)n.QuadPart;
    }
private:
    LARGE_INTEGER freq_{};
    double ticks_ = 0, next_ = 0;
    HANDLE timer_ = nullptr;
};

// =====================================================================
// 5) GPU timer (drives adaptive resolution)
// =====================================================================
class GpuTimer {
public:
    void Init(ID3D11Device* d, ID3D11DeviceContext* c) {
        ctx_ = c;
        D3D11_QUERY_DESC q{};
        for (int i = 0; i < N; ++i) {
            q.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            AH_CHECK(d->CreateQuery(&q, &dj_[i]), "CreateQuery disjoint");
            q.Query = D3D11_QUERY_TIMESTAMP;
            AH_CHECK(d->CreateQuery(&q, &t0_[i]), "CreateQuery ts0");
            AH_CHECK(d->CreateQuery(&q, &t1_[i]), "CreateQuery ts1");
        }
    }
    void Begin() {
        active_ = false;
        if (inflight_ >= N) return;
        ctx_->Begin(dj_[w_].Get());
        ctx_->End(t0_[w_].Get());
        active_ = true;
    }
    void End() {
        if (!active_) return;
        ctx_->End(t1_[w_].Get());
        ctx_->End(dj_[w_].Get());
        w_ = (w_ + 1) % N;
        ++inflight_;
        active_ = false;
    }
    void Poll() {
        while (inflight_ > 0) {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            if (ctx_->GetData(dj_[r_].Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) break;
            UINT64 a = 0, b = 0;
            bool ok = !dj.Disjoint
                && ctx_->GetData(t0_[r_].Get(), &a, sizeof(a), 0) == S_OK
                && ctx_->GetData(t1_[r_].Get(), &b, sizeof(b), 0) == S_OK;
            if (ok && b >= a) {
                double ms = (double)(b - a) / (double)dj.Frequency * 1000.0;
                ema_ = ema_ < 0 ? ms : ema_ * 0.9 + ms * 0.1;
            }
            r_ = (r_ + 1) % N;
            --inflight_;
        }
    }
    double Ms() const { return ema_; }
    void ResetAverage() { ema_ = -1.0; }
private:
    static const int N = 6;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<ID3D11Query> dj_[N], t0_[N], t1_[N];
    int w_ = 0, r_ = 0, inflight_ = 0;
    bool active_ = false;
    double ema_ = -1.0;
};

// =====================================================================
// 6) Screen capture (copies straight into the renderer's frame slot)
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

    // pt = the game's own present time (QPC units), used for exact frame timing
    CapResult Acquire(UINT timeoutMs, ID3D11Texture2D* dst, LONGLONG& pt) {
        pt = 0;
        if (!dup_) return CapResult::Lost;
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> res;
        HRESULT hr = dup_->AcquireNextFrame(timeoutMs, &info, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return CapResult::NoChange;
        if (FAILED(hr)) { dup_.Reset(); return CapResult::Lost; }
        bool got = false;
        if (info.LastPresentTime.QuadPart != 0 && dst) {
            ComPtr<ID3D11Texture2D> t;
            if (SUCCEEDED(res.As(&t))) {
                ctx_->CopyResource(dst, t.Get());
                pt = info.LastPresentTime.QuadPart;
                got = true;
            }
        }
        dup_->ReleaseFrame();
        return got ? CapResult::NewFrame : CapResult::NoChange;
    }

    int Width() const { return w_; }
    int Height() const { return h_; }
    DXGI_FORMAT Format() const { return fmt_; }

private:
    bool Open() {
        dup_.Reset();
        HRESULT hr = out_->DuplicateOutput(dev_.Get(), &dup_);
        if (FAILED(hr)) { Log("DuplicateOutput failed 0x%08X", (unsigned)hr); return false; }
        DXGI_OUTDUPL_DESC dd{};
        dup_->GetDesc(&dd);
        w_ = (int)dd.ModeDesc.Width; h_ = (int)dd.ModeDesc.Height; fmt_ = dd.ModeDesc.Format;
        if (fmt_ != DXGI_FORMAT_B8G8R8A8_UNORM) Log("WARNING: capture format %d (HDR on?)", (int)fmt_);
        Log("capture opened %dx%d", w_, h_);
        return true;
    }

    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGIOutput1> out_;
    ComPtr<IDXGIOutputDuplication> dup_;
    int w_ = 0, h_ = 0;
    DXGI_FORMAT fmt_ = DXGI_FORMAT_UNKNOWN;
};

// =====================================================================
// 7) Renderer
// =====================================================================
struct alignas(16) CBData {
    float srcSize[2]; float dstSize[2];
    float sharp, clarity, vib, black;
    float gam, contrast, oled, frame;
    float P0[4];
    float P1[4];
};

struct Target {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
};

class Renderer {
public:
    void Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, IDXGIFactory2* fac, HWND hwnd, int dw, int dh) {
        dev_ = dev; ctx_ = ctx; dispW_ = dw; dispH_ = dh;

        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = dw; sd.Height = dh; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        ComPtr<IDXGISwapChain1> sc1;
        AH_CHECK(fac->CreateSwapChainForHwnd(dev, hwnd, &sd, nullptr, nullptr, &sc1), "CreateSwapChain");
        AH_CHECK(sc1.As(&sc_), "IDXGISwapChain2 not available");
        sc_->SetMaximumFrameLatency(1);               // lowest input lag: one queued frame at most
        waitable_ = sc_->GetFrameLatencyWaitableObject();
        fac->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        ComPtr<ID3D11Texture2D> bb;
        AH_CHECK(sc_->GetBuffer(0, IID_PPV_ARGS(&bb)), "GetBuffer");
        AH_CHECK(dev->CreateRenderTargetView(bb.Get(), nullptr, &bbRTV_), "Backbuffer RTV");

        auto vsB = Compile("VS", "vs_5_0");
        AH_CHECK(dev->CreateVertexShader(vsB->GetBufferPointer(), vsB->GetBufferSize(), nullptr, &vs_), "VS");
        MakePS("PSUpscale", psUp_);
        MakePS("PSFinal", psFinal_);
        MakePS("PSMotionCoarse", psMotionC_);
        MakePS("PSMotionFine", psMotionF_);
        MakePS("PSInterp", psInterp_);

        D3D11_SAMPLER_DESC ssd{};
        ssd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        ssd.AddressU = ssd.AddressV = ssd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        ssd.MaxLOD = D3D11_FLOAT32_MAX;
        AH_CHECK(dev->CreateSamplerState(&ssd, &samp_), "Sampler");

        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(CBData); bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        AH_CHECK(dev->CreateBuffer(&bd, nullptr, &cb_), "Constant buffer");

        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs_.Get(), nullptr, 0);
        ctx->PSSetSamplers(0, 1, samp_.GetAddressOf());
        ctx->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
    }

    // scale <= 1 : single pass at native resolution (cheapest). scale > 1 : supersampled path.
    void Configure(float scale) {
        int iw = (int)std::lround(dispW_ * scale);
        int ih = (int)std::lround(dispH_ * scale);
        useInternal_ = iw > dispW_ + 1;
        if (!useInternal_) {
            if (a_.tex) Log("internal resolution: native %dx%d", dispW_, dispH_);
            iw_ = dispW_; ih_ = dispH_; a_ = Target{};
            return;
        }
        iw = std::clamp(iw, dispW_, 8192);
        ih = std::clamp(ih, dispH_, 8192);
        if (iw == iw_ && ih == ih_ && a_.tex) return;
        iw_ = iw; ih_ = ih;
        MakeTarget(a_, iw_, ih_, DXGI_FORMAT_R8G8B8A8_UNORM);
        Log("internal resolution %dx%d", iw_, ih_);
    }

    void EnsureSource(int w, int h, DXGI_FORMAT fmt) {
        srcW_ = w; srcH_ = h;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = fmt; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        for (int i = 0; i < 2; ++i) {
            tex_[i].Reset(); srv_[i].Reset();
            AH_CHECK(dev_->CreateTexture2D(&d, nullptr, &tex_[i]), "Create frame texture");
            AH_CHECK(dev_->CreateShaderResourceView(tex_[i].Get(), nullptr, &srv_[i]), "Create frame SRV");
        }
        mw_ = (w + 7) / 8;   mh_ = (h + 7) / 8;
        cw_ = (w + 15) / 16; ch_ = (h + 15) / 16;
        MakeTarget(coarse_, cw_, ch_, DXGI_FORMAT_R16G16B16A16_FLOAT);
        MakeTarget(motion_, mw_, mh_, DXGI_FORMAT_R16G16B16A16_FLOAT);
        MakeTarget(interp_, w, h, DXGI_FORMAT_R8G8B8A8_UNORM);
        cur_ = 0; hist_ = 0; motionValid_ = false;
        Log("source buffers %dx%d, motion grid %dx%d", w, h, mw_, mh_);
    }

    // Two-slot ring: the capture is copied into the older slot, then committed.
    ID3D11Texture2D* NextSlot() { return tex_[cur_ ^ 1].Get(); }
    void Commit() { cur_ ^= 1; if (hist_ < 2) ++hist_; motionValid_ = false; }
    bool HasFrame() const { return hist_ >= 1; }
    bool HasPair() const { return hist_ >= 2; }

    void RenderReal(const Config& cfg, bool oledOn, bool usePrev) {
        Chain(usePrev ? srv_[cur_ ^ 1].Get() : srv_[cur_].Get(), cfg, oledOn);
    }

    // Frame at phase t (0 = previous real frame, 1 = current real frame)
    void RenderPhase(const Config& cfg, bool oledOn, float t) {
        if (t <= 0.03f) { RenderReal(cfg, oledOn, true);  return; }
        if (t >= 0.97f) { RenderReal(cfg, oledOn, false); return; }
        if (!motionValid_) {
            CBData cb{};
            cb.srcSize[0] = (float)srcW_; cb.srcSize[1] = (float)srcH_;
            cb.P0[1] = (float)cfg.fgSearch;
            ID3D11ShaderResourceView* ci[2] = { srv_[cur_ ^ 1].Get(), srv_[cur_].Get() };
            Pass(psMotionC_.Get(), ci, 2, coarse_.rtv.Get(), cw_, ch_, cb);
            ID3D11ShaderResourceView* fi[4] = { srv_[cur_ ^ 1].Get(), srv_[cur_].Get(), nullptr, coarse_.srv.Get() };
            Pass(psMotionF_.Get(), fi, 4, motion_.rtv.Get(), mw_, mh_, cb);
            motionValid_ = true;
        }
        CBData cb{};
        cb.srcSize[0] = (float)srcW_; cb.srcSize[1] = (float)srcH_;
        cb.P0[0] = t; cb.P0[2] = 0.10f;
        cb.P1[0] = 1.0f / (float)mw_; cb.P1[1] = 1.0f / (float)mh_;
        ID3D11ShaderResourceView* ii[3] = { srv_[cur_ ^ 1].Get(), srv_[cur_].Get(), motion_.srv.Get() };
        Pass(psInterp_.Get(), ii, 3, interp_.rtv.Get(), srcW_, srcH_, cb);
        Chain(interp_.srv.Get(), cfg, oledOn);
    }

    void WaitFrame() { if (waitable_) WaitForSingleObjectEx(waitable_, 100, TRUE); }
    void Present(UINT interval) { sc_->Present(interval, 0); }

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

    void MakePS(const char* entry, ComPtr<ID3D11PixelShader>& out) {
        auto b = Compile(entry, "ps_5_0");
        AH_CHECK(dev_->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &out), entry);
    }

    void MakeTarget(Target& t, int w, int h, DXGI_FORMAT fmt) {
        t = Target{};
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = fmt; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        AH_CHECK(dev_->CreateTexture2D(&d, nullptr, &t.tex), "Create target texture");
        AH_CHECK(dev_->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv), "Create target RTV");
        AH_CHECK(dev_->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv), "Create target SRV");
    }

    void Pass(ID3D11PixelShader* ps, ID3D11ShaderResourceView* const* srvs, UINT n,
              ID3D11RenderTargetView* rtv, int w, int h, const CBData& cb) {
        ctx_->UpdateSubresource(cb_.Get(), 0, nullptr, &cb, 0, 0);
        D3D11_VIEWPORT vp{0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
        ctx_->RSSetViewports(1, &vp);
        ctx_->OMSetRenderTargets(1, &rtv, nullptr);
        ctx_->PSSetShader(ps, nullptr, 0);
        ctx_->PSSetShaderResources(0, n, srvs);
        ctx_->Draw(3, 0);
        ID3D11ShaderResourceView* none[4] = {nullptr, nullptr, nullptr, nullptr};
        ctx_->PSSetShaderResources(0, n, none);
    }

    void Chain(ID3D11ShaderResourceView* src, const Config& cfg, bool oledOn) {
        CBData cb{};
        cb.sharp = cfg.sharpness; cb.clarity = cfg.clarity; cb.vib = cfg.saturation;
        cb.black = cfg.blackLevel; cb.gam = cfg.gamma; cb.contrast = cfg.contrast;
        cb.oled = oledOn ? 1.0f : 0.0f;
        cb.frame = (float)(frameNo_++ % 1024);
        cb.dstSize[0] = (float)dispW_; cb.dstSize[1] = (float)dispH_;

        ID3D11ShaderResourceView* in = src;
        cb.srcSize[0] = (float)srcW_; cb.srcSize[1] = (float)srcH_;
        if (useInternal_) {
            CBData up = cb;
            up.dstSize[0] = (float)iw_; up.dstSize[1] = (float)ih_;
            ID3D11ShaderResourceView* s0[1] = { src };
            Pass(psUp_.Get(), s0, 1, a_.rtv.Get(), iw_, ih_, up);
            in = a_.srv.Get();
            cb.srcSize[0] = (float)iw_; cb.srcSize[1] = (float)ih_;
        }
        ID3D11ShaderResourceView* s1[1] = { in };
        Pass(psFinal_.Get(), s1, 1, bbRTV_.Get(), dispW_, dispH_, cb);
    }

    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain2> sc_;
    HANDLE waitable_ = nullptr;
    ComPtr<ID3D11RenderTargetView> bbRTV_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> psUp_, psFinal_, psMotionC_, psMotionF_, psInterp_;
    ComPtr<ID3D11SamplerState> samp_;
    ComPtr<ID3D11Buffer> cb_;
    Target a_, coarse_, motion_, interp_;
    ComPtr<ID3D11Texture2D> tex_[2];
    ComPtr<ID3D11ShaderResourceView> srv_[2];
    int dispW_ = 0, dispH_ = 0, iw_ = 0, ih_ = 0;
    int srcW_ = 0, srcH_ = 0, mw_ = 0, mh_ = 0, cw_ = 0, ch_ = 0;
    int cur_ = 0, hist_ = 0, frameNo_ = 0;
    bool useInternal_ = false, motionValid_ = false;
};

// =====================================================================
// 8) Main
// =====================================================================
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"AH_Upscaler_Single_Instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    SetProcessDPIAware();

    const std::string dir = ExeDir();
    LogInit(dir + "upscaler.log");
    Log("AH Upscaler Phase 4 (Smooth) starting");

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

    DXGI_ADAPTER_DESC1 ad1{};
    adapter->GetDesc1(&ad1);
    Log("GPU: %ls (%u MB VRAM)", ad1.Description, (unsigned)(ad1.DedicatedVideoMemory / (1024 * 1024)));

    DXGI_OUTPUT_DESC od{};
    output->GetDesc(&od);
    const RECT rc = od.DesktopCoordinates;
    const int W = rc.right - rc.left, H = rc.bottom - rc.top;
    Log("monitor %d: %dx%d at (%ld,%ld)", cfg.monitor, W, H, rc.left, rc.top);

    DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    int hz = 60;
    if (EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) hz = (int)dm.dmDisplayFrequency;
    Log("display %d Hz", hz);

    int syncInterval = 0;
    bool vsyncPaced = false;
    auto calcSync = [&]() {
        syncInterval = (hz > 0 && hz % cfg.targetFps == 0) ? hz / cfg.targetFps : 0;
        vsyncPaced = syncInterval >= 1;
        Log("pacing: %s (interval %d)", vsyncPaced ? "vsync" : "timer", syncInterval);
    };
    calcSync();

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

    const struct { int id; UINT vk; const char* name; } keys[] = {
        {1, VK_F8, "F8"}, {2, VK_F9, "F9"}, {3, VK_F10, "F10"},
        {4, VK_F11, "F11"}, {5, VK_F7, "F7"}, {6, VK_F6, "F6"}
    };
    for (auto& k : keys)
        if (!RegisterHotKey(hwnd, k.id, MOD_NOREPEAT, k.vk)) Log("WARNING: hotkey %s already used by another app", k.name);

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    AH_CHECK(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, &dev, nullptr, &ctx), "D3D11CreateDevice");
    {
        ComPtr<IDXGIDevice> dxgiDev;
        if (SUCCEEDED(dev.As(&dxgiDev))) {
            HRESULT hr = dxgiDev->SetGPUThreadPriority(7);   // let our short passes jump ahead of the game
            Log("SetGPUThreadPriority -> 0x%08X", (unsigned)hr);
        }
    }
    ComPtr<IDXGIFactory2> f2;
    AH_CHECK(f1.As(&f2), "IDXGIFactory2 not available");

    Capture capture;
    if (!capture.Init(dev.Get(), ctx.Get(), output.Get()))
        Fatal("Desktop capture failed. Close other capture apps, or the monitor is on another GPU.");

    Renderer renderer;
    renderer.Init(dev.Get(), ctx.Get(), f2.Get(), hwnd, W, H);
    bool nativeOnly = false;
    float curScale = cfg.scale;
    renderer.Configure(curScale);
    renderer.EnsureSource(capture.Width(), capture.Height(), capture.Format());

    GpuTimer gpu;
    gpu.Init(dev.Get(), ctx.Get());

    timeBeginPeriod(1);
    FramePacer pacer;
    pacer.Init(cfg.targetFps);

    LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
    const double qfd = (double)qf.QuadPart;
    auto NowQ = [&]() { LARGE_INTEGER n; QueryPerformanceCounter(&n); return (LONGLONG)n.QuadPart; };

    LONGLONG lastPT = 0, currPT = 0, lastPresentQ = 0;
    double srcInterval = 1.0 / (double)cfg.targetFps;

    bool running = true, paused = false, oledOn = cfg.oled, fgOn = cfg.framegen;
    bool fgActive = false, curShown = false, dirty = false;
    int adaptTick = 0;
    unsigned long long statStart = GetTickCount64();
    int presented = 0, captured = 0;

    while (running) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) { running = false; break; }
            if (m.message == WM_HOTKEY) {
                switch (m.wParam) {
                case 1:   // reload config
                    LoadConfig(cfg, iniPath);
                    curScale = nativeOnly ? 1.0f : cfg.scale;
                    renderer.Configure(curScale);
                    pacer.Init(cfg.targetFps);
                    calcSync();
                    oledOn = cfg.oled; fgOn = cfg.framegen;
                    gpu.ResetAverage(); dirty = true;
                    break;
                case 2: oledOn = !oledOn; dirty = true; Log("OLED %s", oledOn ? "ON" : "OFF"); break;
                case 3: running = false; break;
                case 4:
                    paused = !paused;
                    ShowWindow(hwnd, paused ? SW_HIDE : SW_SHOWNOACTIVATE);
                    pacer.Reset();
                    break;
                case 5: fgOn = !fgOn; Log("frame generation %s", fgOn ? "ON" : "OFF"); break;
                case 6:   // quick switch: native resolution <-> supersampled
                    nativeOnly = !nativeOnly;
                    curScale = nativeOnly ? 1.0f : cfg.scale;
                    renderer.Configure(curScale);
                    gpu.ResetAverage(); dirty = true;
                    Log("scale -> %.2f (%s)", curScale, nativeOnly ? "native" : "supersampled");
                    break;
                }
            }
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (!running) break;
        if (paused) { Sleep(50); continue; }

        const double tick = 1.0 / (double)cfg.targetFps;

        // In generated mode the display tick drives the loop
        if (fgActive) {
            if (vsyncPaced) renderer.WaitFrame(); else pacer.Wait();
        }

        // ---- acquire newest source frame(s) ----
        bool got = false, lost = false;
        const int maxDrain = fgActive ? 4 : 1;
        for (int k = 0; k < maxDrain; ++k) {
            LONGLONG pt = 0;
            const UINT to = (!fgActive && k == 0) ? 8u : 0u;
            CapResult r = capture.Acquire(to, renderer.NextSlot(), pt);
            if (r == CapResult::Lost) { lost = true; break; }
            if (r != CapResult::NewFrame) break;
            renderer.Commit();
            got = true; ++captured; curShown = false;
            if (lastPT != 0) {
                double dt = (double)(pt - lastPT) / qfd;
                if (dt > 0.002 && dt < 0.25) srcInterval = srcInterval * 0.8 + dt * 0.2;
            }
            lastPT = pt; currPT = pt;
        }
        if (lost) {
            Sleep(100);
            if (capture.Recreate()) {
                renderer.EnsureSource(capture.Width(), capture.Height(), capture.Format());
                lastPT = 0; fgActive = false;
            }
            continue;
        }

        // ---- choose mode: native passthrough (lowest lag) or generated 60 ----
        const double ratio = srcInterval / tick;
        if (!fgActive && fgOn && renderer.HasPair() && ratio > 1.15 && ratio < 3.2) {
            fgActive = true; pacer.Reset();
            Log("mode -> GENERATED (source %.1f fps)", 1.0 / srcInterval);
        } else if (fgActive && (!fgOn || ratio < 1.06 || ratio > 4.0)) {
            fgActive = false;
            Log("mode -> PASSTHROUGH (source %.1f fps)", 1.0 / srcInterval);
        }

        // ---- render ----
        if (!fgActive) {
            if (renderer.HasFrame() && (got || dirty)) {
                const double since = (double)(NowQ() - lastPresentQ) / qfd;
                if (since >= tick * 0.9 || !got) {
                    gpu.Begin();
                    renderer.RenderReal(cfg, oledOn, false);
                    gpu.End();
                    renderer.Present(0);
                    lastPresentQ = NowQ();
                    ++presented; ++adaptTick;
                }
            }
        } else if (renderer.HasPair()) {
            double t = ((double)(NowQ() - currPT) / qfd) / srcInterval;
            t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
            const bool skip = (!got && !dirty && curShown && t >= 0.97);
            if (!skip) {
                gpu.Begin();
                renderer.RenderPhase(cfg, oledOn, (float)t);
                gpu.End();
                renderer.Present(vsyncPaced ? (UINT)syncInterval : 0u);
                curShown = (t >= 0.97);
                lastPresentQ = NowQ();
                ++presented; ++adaptTick;
            } else {
                Sleep(1);
            }
        }
        dirty = false;

        // ---- adaptive resolution: keep our GPU cost small so the game keeps its frames ----
        gpu.Poll();
        if (cfg.adaptive && !nativeOnly && adaptTick >= 90) {
            adaptTick = 0;
            const double ms = gpu.Ms();
            if (ms > 0.0) {
                const double budget = tick * 1000.0;
                float ns = curScale;
                if (ms > budget * 0.45) ns = curScale - 0.0833f;
                else if (ms < budget * 0.22 && curScale < cfg.scale) ns = curScale + 0.0833f;
                ns = Clampf(ns, cfg.minScale, cfg.scale);
                if (ns < 1.02f) ns = 1.0f;
                if (std::fabs(ns - curScale) > 0.01f) {
                    curScale = ns;
                    renderer.Configure(curScale);
                    gpu.ResetAverage(); dirty = true;
                    Log("adaptive scale -> %.2f (our gpu cost %.2f ms, frame budget %.2f ms)", curScale, ms, budget);
                }
            }
        }

        const unsigned long long nowMs = GetTickCount64();
        if (nowMs - statStart >= 5000) {
            const double s = (double)(nowMs - statStart) / 1000.0;
            Log("present %.1f fps | source %.1f fps | %s | gpu %.2f ms | scale %.2f",
                presented / s, captured / s, fgActive ? "GENERATED" : "PASSTHROUGH", gpu.Ms(), curScale);
            presented = captured = 0;
            statStart = nowMs;
        }
    }

    timeEndPeriod(1);
    Log("exit");
    if (g_log) fclose(g_log);
    if (mutex) CloseHandle(mutex);
    return 0;
}
