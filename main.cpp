// =====================================================================
//  AH Upscaler - Phase 3
//  (Hierarchical FrameGen + Presets + Edge Smoothing + Adaptive Resolution)
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
#include <utility>
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
// 2) Config + presets
// =====================================================================
struct Config {
    int   monitor = 0;
    int   targetFps = 60;
    int   vsync = -1;          // -1 auto, 0 off, 1 on
    int   preset = 0;          // 0 custom, 1 quality, 2 balanced, 3 performance
    bool  oled = true;
    bool  layered = true;      // set 0 if you get a black screen
    float scale = 2.0f;        // max internal res multiplier (2.0 = 4K on 1080p)
    float minScale = 1.0f;     // adaptive lower bound
    bool  adaptive = true;
    bool  framegen = true;
    int   fgSearch = 16;       // motion search range (even number, 4..32)
    float aa = 0.5f;           // edge smoothing 0..1
    float sharpness = 0.6f;
    float clarity = 0.35f;
    float saturation = 1.15f;
    float blackLevel = 0.02f;
    float gamma = 1.12f;
    float contrast = 0.35f;
};

static const char* PresetName(int p) {
    switch (p) { case 1: return "Quality"; case 2: return "Balanced"; case 3: return "Performance"; default: return "Custom"; }
}

static void ApplyPreset(Config& c, int p) {
    switch (p) {
    case 1: c.scale = 2.00f; c.minScale = 1.25f; c.fgSearch = 16; c.sharpness = 0.60f; c.aa = 0.50f; break;
    case 2: c.scale = 1.50f; c.minScale = 1.00f; c.fgSearch = 12; c.sharpness = 0.55f; c.aa = 0.40f; break;
    case 3: c.scale = 1.25f; c.minScale = 1.00f; c.fgSearch = 8;  c.sharpness = 0.50f; c.aa = 0.30f; break;
    default: break;
    }
    c.preset = p;
}

static std::string ExeDir() {
    char p[MAX_PATH];
    GetModuleFileNameA(nullptr, p, MAX_PATH);
    std::string s(p);
    return s.substr(0, s.find_last_of("\\/") + 1);
}

static float Clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

struct IniDefault { const char* key; const char* val; };
static const IniDefault kDefaults[] = {
    {"monitor", "0"}, {"target_fps", "60"}, {"vsync", "-1"}, {"preset", "0"}, {"oled", "1"}, {"layered", "1"},
    {"scale", "2.0"}, {"min_scale", "1.0"}, {"adaptive", "1"}, {"framegen", "1"}, {"fg_search", "16"},
    {"aa", "0.5"}, {"sharpness", "0.6"}, {"clarity", "0.35"}, {"saturation", "1.15"},
    {"black_level", "0.02"}, {"gamma", "1.12"}, {"contrast", "0.35"}
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
    c.monitor    = std::max(0, I("monitor", "0"));
    c.targetFps  = std::clamp(I("target_fps", "60"), 24, 240);
    c.vsync      = I("vsync", "-1");
    c.oled       = I("oled", "1") != 0;
    c.layered    = I("layered", "1") != 0;
    c.scale      = Clampf(F("scale", "2.0"), 1.0f, 2.0f);
    c.minScale   = Clampf(F("min_scale", "1.0"), 1.0f, c.scale);
    c.adaptive   = I("adaptive", "1") != 0;
    c.framegen   = I("framegen", "1") != 0;
    c.fgSearch   = std::clamp(I("fg_search", "16"), 4, 32) & ~1;
    c.aa         = Clampf(F("aa", "0.5"), 0.0f, 1.0f);
    c.sharpness  = Clampf(F("sharpness", "0.6"), 0.0f, 1.0f);
    c.clarity    = Clampf(F("clarity", "0.35"), 0.0f, 1.0f);
    c.saturation = Clampf(F("saturation", "1.15"), 0.0f, 2.0f);
    c.blackLevel = Clampf(F("black_level", "0.02"), 0.0f, 0.2f);
    c.gamma      = Clampf(F("gamma", "1.12"), 0.5f, 2.0f);
    c.contrast   = Clampf(F("contrast", "0.35"), 0.0f, 1.0f);
    c.preset     = std::clamp(I("preset", "0"), 0, 3);
    if (c.preset != 0) ApplyPreset(c, c.preset);
    Log("config: monitor=%d fps=%d preset=%s scale=%.2f..%.2f adaptive=%d framegen=%d search=%d aa=%.2f oled=%d",
        c.monitor, c.targetFps, PresetName(c.preset), c.minScale, c.scale,
        (int)c.adaptive, (int)c.framegen, c.fgSearch, c.aa, (int)c.oled);
}

// =====================================================================
// 3) Shaders (HLSL)
// =====================================================================
static const char* kShaderSrc = R"HLSL(
cbuffer C : register(b0) {
    float2 srcSize; float2 dstSize;
    float sharp; float clarity; float sat; float black;
    float gam; float contrast; float oled; float pad;
    float4 extra;    // x = interp t, z = search range, w = SAD threshold
    float4 extra2;   // xy = 1 / motion size, z = edge smoothing strength
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
// Result: displacement from a current-frame block to its match in the previous frame.

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

// Coarse level: 16x16 blocks, samples every 4 px, search step 4 px. Output in PIXELS.
float4 PSMotionCoarse(VSOut i) : SV_Target {
    float2 base = floor(i.p.xy) * 16.0;
    float cl[16];
    [unroll] for (int j = 0; j < 4; j++) {
        [unroll] for (int k = 0; k < 4; k++) {
            float2 pp = base + (float2(k, j) + 0.5) * 4.0;
            cl[j * 4 + k] = Luma(gB.SampleLevel(gS, pp / srcSize, 0).rgb);
        }
    }
    float best = BlockSAD(base, float2(0, 0), 4.0, cl) - 0.004;   // bias toward zero motion
    float2 bd = float2(0, 0);
    int n = (int)(extra.z * 0.5);
    [loop] for (int y = -n; y <= n; y++) {
        [loop] for (int x = -n; x <= n; x++) {
            float2 d = float2(x, y) * 4.0;
            float s = BlockSAD(base, d, 4.0, cl);
            if (s < best) { best = s; bd = d; }
        }
    }
    return float4(bd, max(best, 0.0), 1);
}

// Fine level: 8x8 blocks, refine around the coarse vector. Output in UV.
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
    float s0 = BlockSAD(base, cv, 2.0, cl);
    float sz = BlockSAD(base, float2(0, 0), 2.0, cl);
    float best = s0;
    float2 bd = cv;
    if (sz - 0.004 < s0) { best = sz; bd = float2(0, 0); }

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

// ---------------- Motion-compensated interpolation ----------------
float4 PSInterp(VSOut i) : SV_Target {
    float t = extra.x;
    float4 m = gM.SampleLevel(gS, i.uv, 0);
    float2 d = m.xy;
    float conf = 1.0 - smoothstep(extra.w * 0.5, extra.w, m.z);

    // motion discontinuity check against neighbouring blocks (reduces ghosting)
    float2 mt = extra2.xy;
    float2 a1 = gM.SampleLevel(gS, i.uv + float2( mt.x, 0), 0).xy;
    float2 a2 = gM.SampleLevel(gS, i.uv + float2(-mt.x, 0), 0).xy;
    float2 a3 = gM.SampleLevel(gS, i.uv + float2(0,  mt.y), 0).xy;
    float2 a4 = gM.SampleLevel(gS, i.uv + float2(0, -mt.y), 0).xy;
    float disc = max(max(length((a1 - d) * srcSize), length((a2 - d) * srcSize)),
                     max(length((a3 - d) * srcSize), length((a4 - d) * srcSize)));
    conf *= 1.0 - smoothstep(2.0, 8.0, disc);

    float3 p0 = gA.SampleLevel(gS, i.uv + d * t, 0).rgb;
    float3 c0 = gB.SampleLevel(gS, i.uv - d * (1.0 - t), 0).rgb;
    float3 warped = lerp(p0, c0, t);
    float3 plain  = lerp(gA.SampleLevel(gS, i.uv, 0).rgb, gB.SampleLevel(gS, i.uv, 0).rgb, t);
    float diff = length(p0 - c0);
    float ok = conf * (1.0 - smoothstep(0.15, 0.40, diff));
    return float4(lerp(plain, warped, ok), 1);
}

// ---------------- Upscale / edge smoothing / sharpen / final ----------------
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

// FXAA-lite: smooths along the luma gradient where an edge is detected
float4 PSEdge(VSOut i) : SV_Target {
    float2 t = 1.0 / srcSize;
    float3 c = gA.SampleLevel(gS, i.uv, 0).rgb;
    float lc = Luma(c);
    float ln = Luma(gA.SampleLevel(gS, i.uv + float2(0, -t.y), 0).rgb);
    float ls = Luma(gA.SampleLevel(gS, i.uv + float2(0,  t.y), 0).rgb);
    float lw = Luma(gA.SampleLevel(gS, i.uv + float2(-t.x, 0), 0).rgb);
    float le = Luma(gA.SampleLevel(gS, i.uv + float2( t.x, 0), 0).rgb);
    float lmin = min(lc, min(min(ln, ls), min(lw, le)));
    float lmax = max(lc, max(max(ln, ls), max(lw, le)));
    float range = lmax - lmin;
    if (range < max(0.04, lmax * 0.15)) return float4(c, 1);
    float2 g = float2(le - lw, ls - ln);
    float len = length(g);
    float2 dir = len > 1e-5 ? g / len : float2(1, 0);
    float3 a = gA.SampleLevel(gS, i.uv + dir * t * 0.5, 0).rgb;
    float3 b = gA.SampleLevel(gS, i.uv - dir * t * 0.5, 0).rgb;
    float3 sm = (a + b + c * 2.0) * 0.25;
    float blend = saturate(range * 2.5) * extra2.z;
    return float4(lerp(c, sm, blend), 1);
}

float4 PSSharpen(VSOut i) : SV_Target {
    float2 t = 1.0 / srcSize;
    float3 c = gA.SampleLevel(gS, i.uv, 0).rgb;
    float3 n = gA.SampleLevel(gS, i.uv + float2(0, -t.y), 0).rgb;
    float3 s = gA.SampleLevel(gS, i.uv + float2(0,  t.y), 0).rgb;
    float3 w = gA.SampleLevel(gS, i.uv + float2(-t.x, 0), 0).rgb;
    float3 e = gA.SampleLevel(gS, i.uv + float2( t.x, 0), 0).rgb;
    float3 mn = min(c, min(min(n, s), min(w, e)));
    float3 mx = max(c, max(max(n, s), max(w, e)));
    float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));
    float peak = -1.0 / lerp(8.0, 5.0, sharp);
    float3 k = amp * peak;
    float3 r = (k * (n + s + w + e) + c) / (1.0 + 4.0 * k);
    return float4(saturate(r), 1);
}

float4 PSFinal(VSOut i) : SV_Target {
    float2 o = 0.25 / dstSize;
    float3 c = 0.25 * (
        gA.SampleLevel(gS, i.uv + float2(-o.x, -o.y), 0).rgb +
        gA.SampleLevel(gS, i.uv + float2( o.x, -o.y), 0).rgb +
        gA.SampleLevel(gS, i.uv + float2(-o.x,  o.y), 0).rgb +
        gA.SampleLevel(gS, i.uv + float2( o.x,  o.y), 0).rgb);

    if (clarity > 0.001) {
        float2 r = 2.0 / srcSize;
        float3 blur = 0.25 * (
            gA.SampleLevel(gS, i.uv + float2( r.x, 0), 0).rgb +
            gA.SampleLevel(gS, i.uv + float2(-r.x, 0), 0).rgb +
            gA.SampleLevel(gS, i.uv + float2(0,  r.y), 0).rgb +
            gA.SampleLevel(gS, i.uv + float2(0, -r.y), 0).rgb);
        c = saturate(c + (c - blur) * clarity * 1.2);
        float l = Luma(c);
        c *= 1.0 + 0.12 * clarity * smoothstep(0.6, 1.0, l);
        c *= 0.92 + 0.08 * smoothstep(0.0, 0.5, l);
    }

    if (oled > 0.5) {
        float l = Luma(c);
        c = lerp(l.xxx, c, sat);
        c = saturate((c - black) / (1.0 - black));
        c = pow(c, gam);
        c = lerp(c, c * c * (3.0 - 2.0 * c), contrast);
    }

    float nz = frac(sin(dot(i.p.xy, float2(12.9898, 78.233))) * 43758.5453);
    c += (nz - 0.5) / 255.0;
    return float4(saturate(c), 1);
}
)HLSL";

// =====================================================================
// 4) Frame pacer
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
    void Reset() {
        LARGE_INTEGER n; QueryPerformanceCounter(&n);
        next_ = (double)n.QuadPart;
    }
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
// 5) GPU timer (for adaptive resolution)
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
// 6) Screen capture (Desktop Duplication)
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

    CapResult Acquire() {
        if (!dup_) return CapResult::Lost;
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> res;
        HRESULT hr = dup_->AcquireNextFrame(0, &info, &res);
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
    DXGI_FORMAT Format() const { return fmt_; }

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
// 7) Renderer
// =====================================================================
struct alignas(16) CBData {
    float srcSize[2]; float dstSize[2];
    float sharp, clarity, sat, black;
    float gam, contrast, oled, pad;
    float extra[4];
    float extra2[4];
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
        AH_CHECK(fac->CreateSwapChainForHwnd(dev, hwnd, &sd, nullptr, nullptr, &sc_), "CreateSwapChain");
        fac->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        ComPtr<ID3D11Texture2D> bb;
        AH_CHECK(sc_->GetBuffer(0, IID_PPV_ARGS(&bb)), "GetBuffer");
        AH_CHECK(dev->CreateRenderTargetView(bb.Get(), nullptr, &bbRTV_), "Backbuffer RTV");

        auto vsB = Compile("VS", "vs_5_0");
        AH_CHECK(dev->CreateVertexShader(vsB->GetBufferPointer(), vsB->GetBufferSize(), nullptr, &vs_), "VS");
        MakePS("PSUpscale", psUp_);
        MakePS("PSEdge", psEdge_);
        MakePS("PSSharpen", psSharp_);
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

    void Configure(float scale) {
        int iw = std::clamp((int)std::lround(dispW_ * scale), dispW_, 8192);
        int ih = std::clamp((int)std::lround(dispH_ * scale), dispH_, 8192);
        if (iw == iw_ && ih == ih_ && a_.tex) return;
        iw_ = iw; ih_ = ih;
        MakeTarget(a_, iw_, ih_, DXGI_FORMAT_R8G8B8A8_UNORM);
        MakeTarget(b_, iw_, ih_, DXGI_FORMAT_R8G8B8A8_UNORM);
        Log("internal resolution %dx%d", iw_, ih_);
    }

    void EnsureSource(int w, int h, DXGI_FORMAT fmt) {
        if (prevTex_ && srcW_ == w && srcH_ == h && srcFmt_ == fmt) return;
        srcW_ = w; srcH_ = h; srcFmt_ = fmt;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = fmt; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        prevTex_.Reset(); currTex_.Reset(); prevSRV_.Reset(); currSRV_.Reset();
        AH_CHECK(dev_->CreateTexture2D(&d, nullptr, &prevTex_), "Create prev frame");
        AH_CHECK(dev_->CreateTexture2D(&d, nullptr, &currTex_), "Create curr frame");
        AH_CHECK(dev_->CreateShaderResourceView(prevTex_.Get(), nullptr, &prevSRV_), "prev SRV");
        AH_CHECK(dev_->CreateShaderResourceView(currTex_.Get(), nullptr, &currSRV_), "curr SRV");
        mw_ = (w + 7) / 8;   mh_ = (h + 7) / 8;
        cw_ = (w + 15) / 16; ch_ = (h + 15) / 16;
        MakeTarget(coarse_, cw_, ch_, DXGI_FORMAT_R16G16B16A16_FLOAT);
        MakeTarget(motion_, mw_, mh_, DXGI_FORMAT_R16G16B16A16_FLOAT);
        MakeTarget(interp_, w, h, DXGI_FORMAT_R8G8B8A8_UNORM);
        hist_ = 0;
        Log("source buffers %dx%d, motion %dx%d, coarse %dx%d", w, h, mw_, mh_, cw_, ch_);
    }

    void PushFrame(ID3D11Texture2D* captured) {
        if (hist_ == 0) ctx_->CopyResource(prevTex_.Get(), captured);
        else            ctx_->CopyResource(prevTex_.Get(), currTex_.Get());
        ctx_->CopyResource(currTex_.Get(), captured);
        if (hist_ < 2) ++hist_;
    }
    bool HasFrame() const { return hist_ >= 1; }
    bool HasPair() const { return hist_ >= 2; }

    void RenderReal(const Config& cfg, bool oledOn) {
        Chain(currSRV_.Get(), srcW_, srcH_, cfg, oledOn);
    }

    void RenderInterp(const Config& cfg, bool oledOn, float t) {
        CBData cb{};
        cb.srcSize[0] = (float)srcW_; cb.srcSize[1] = (float)srcH_;
        cb.extra[0] = t; cb.extra[2] = (float)cfg.fgSearch; cb.extra[3] = 0.10f;
        cb.extra2[0] = 1.0f / (float)mw_; cb.extra2[1] = 1.0f / (float)mh_;

        ID3D11ShaderResourceView* coarseIn[2] = { prevSRV_.Get(), currSRV_.Get() };
        Pass(psMotionC_.Get(), coarseIn, 2, coarse_.rtv.Get(), cw_, ch_, cb);

        ID3D11ShaderResourceView* fineIn[4] = { prevSRV_.Get(), currSRV_.Get(), nullptr, coarse_.srv.Get() };
        Pass(psMotionF_.Get(), fineIn, 4, motion_.rtv.Get(), mw_, mh_, cb);

        ID3D11ShaderResourceView* interpIn[3] = { prevSRV_.Get(), currSRV_.Get(), motion_.srv.Get() };
        Pass(psInterp_.Get(), interpIn, 3, interp_.rtv.Get(), srcW_, srcH_, cb);

        Chain(interp_.srv.Get(), srcW_, srcH_, cfg, oledOn);
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

    void Chain(ID3D11ShaderResourceView* src, int sw, int sh, const Config& cfg, bool oledOn) {
        CBData cb{};
        cb.srcSize[0] = (float)sw; cb.srcSize[1] = (float)sh;
        cb.dstSize[0] = (float)iw_; cb.dstSize[1] = (float)ih_;
        ID3D11ShaderResourceView* s0[1] = { src };
        Pass(psUp_.Get(), s0, 1, a_.rtv.Get(), iw_, ih_, cb);

        Target* cur = &a_;
        Target* oth = &b_;
        cb.srcSize[0] = (float)iw_; cb.srcSize[1] = (float)ih_;

        if (cfg.aa > 0.01f) {
            cb.extra2[2] = cfg.aa;
            ID3D11ShaderResourceView* s1[1] = { cur->srv.Get() };
            Pass(psEdge_.Get(), s1, 1, oth->rtv.Get(), iw_, ih_, cb);
            std::swap(cur, oth);
        }
        if (cfg.sharpness > 0.001f) {
            cb.sharp = cfg.sharpness;
            ID3D11ShaderResourceView* s2[1] = { cur->srv.Get() };
            Pass(psSharp_.Get(), s2, 1, oth->rtv.Get(), iw_, ih_, cb);
            std::swap(cur, oth);
        }

        cb.dstSize[0] = (float)dispW_; cb.dstSize[1] = (float)dispH_;
        cb.clarity = cfg.clarity; cb.sat = cfg.saturation; cb.black = cfg.blackLevel;
        cb.gam = cfg.gamma; cb.contrast = cfg.contrast; cb.oled = oledOn ? 1.0f : 0.0f;
        ID3D11ShaderResourceView* s3[1] = { cur->srv.Get() };
        Pass(psFinal_.Get(), s3, 1, bbRTV_.Get(), dispW_, dispH_, cb);
    }

    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain1> sc_;
    ComPtr<ID3D11RenderTargetView> bbRTV_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> psUp_, psEdge_, psSharp_, psFinal_, psMotionC_, psMotionF_, psInterp_;
    ComPtr<ID3D11SamplerState> samp_;
    ComPtr<ID3D11Buffer> cb_;
    Target a_, b_, coarse_, motion_, interp_;
    ComPtr<ID3D11Texture2D> prevTex_, currTex_;
    ComPtr<ID3D11ShaderResourceView> prevSRV_, currSRV_;
    int dispW_ = 0, dispH_ = 0, iw_ = 0, ih_ = 0;
    int srcW_ = 0, srcH_ = 0, mw_ = 0, mh_ = 0, cw_ = 0, ch_ = 0, hist_ = 0;
    DXGI_FORMAT srcFmt_ = DXGI_FORMAT_UNKNOWN;
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

    SetProcessDPIAware();
    const std::string dir = ExeDir();
    LogInit(dir + "upscaler.log");
    Log("AH Upscaler Phase 3 starting");

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
    const bool vsync = cfg.vsync < 0 ? (hz <= cfg.targetFps + 1) : (cfg.vsync != 0);
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

    const struct { int id; UINT vk; const char* name; } keys[] = {
        {1, VK_F8, "F8"}, {2, VK_F9, "F9"}, {3, VK_F10, "F10"},
        {4, VK_F11, "F11"}, {5, VK_F7, "F7"}, {6, VK_F6, "F6"}
    };
    for (auto& k : keys)
        if (!RegisterHotKey(hwnd, k.id, MOD_NOREPEAT, k.vk)) Log("WARNING: hotkey %s is already used by another app", k.name);

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    AH_CHECK(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, &dev, nullptr, &ctx), "D3D11CreateDevice");
    ComPtr<IDXGIFactory2> f2;
    AH_CHECK(f1.As(&f2), "IDXGIFactory2 not available");

    Capture capture;
    if (!capture.Init(dev.Get(), ctx.Get(), output.Get()))
        Fatal("Desktop capture failed. Close other capture apps, or the monitor is on another GPU.");

    Renderer renderer;
    renderer.Init(dev.Get(), ctx.Get(), f2.Get(), hwnd, W, H);
    float curScale = cfg.scale;
    renderer.Configure(curScale);
    renderer.EnsureSource(capture.Width(), capture.Height(), capture.Format());

    GpuTimer gpu;
    gpu.Init(dev.Get(), ctx.Get());

    timeBeginPeriod(1);
    FramePacer pacer;
    pacer.Init(cfg.targetFps);

    LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
    LONGLONG lastNew = 0;
    double srcInterval = 1.0 / (double)cfg.targetFps;

    bool running = true, paused = false, oledOn = cfg.oled, fgOn = cfg.framegen;
    bool dirty = false, pendingReal = false;
    int adaptTick = 0;
    unsigned long long statStart = GetTickCount64();
    int presented = 0, captured = 0, generated = 0;

    while (running) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) { running = false; break; }
            if (m.message == WM_HOTKEY) {
                switch (m.wParam) {
                case 1:   // reload config
                    LoadConfig(cfg, iniPath);
                    curScale = cfg.scale;
                    renderer.Configure(curScale);
                    pacer.Init(cfg.targetFps);
                    oledOn = cfg.oled; fgOn = cfg.framegen;
                    gpu.ResetAverage();
                    dirty = true;
                    break;
                case 2: oledOn = !oledOn; dirty = true; break;
                case 3: running = false; break;
                case 4:
                    paused = !paused;
                    ShowWindow(hwnd, paused ? SW_HIDE : SW_SHOWNOACTIVATE);
                    pacer.Reset();
                    break;
                case 5: fgOn = !fgOn; Log("frame generation %s", fgOn ? "ON" : "OFF"); break;
                case 6:   // cycle presets
                    cfg.preset = cfg.preset % 3 + 1;
                    ApplyPreset(cfg, cfg.preset);
                    curScale = cfg.scale;
                    renderer.Configure(curScale);
                    gpu.ResetAverage();
                    dirty = true;
                    Log("preset -> %s (scale %.2f..%.2f, search %d, aa %.2f)",
                        PresetName(cfg.preset), cfg.minScale, cfg.scale, cfg.fgSearch, cfg.aa);
                    break;
                }
            }
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (!running) break;
        if (paused) { Sleep(50); continue; }

        CapResult r = capture.Acquire();
        if (r == CapResult::Lost) {
            Sleep(100);
            if (capture.Recreate())
                renderer.EnsureSource(capture.Width(), capture.Height(), capture.Format());
            continue;
        }

        const bool newFrame = (r == CapResult::NewFrame);
        if (newFrame) {
            renderer.PushFrame(capture.Tex());
            ++captured;
            LARGE_INTEGER n; QueryPerformanceCounter(&n);
            if (lastNew != 0) {
                double dt = (double)(n.QuadPart - lastNew) / (double)qf.QuadPart;
                if (dt > 0.001 && dt < 0.5) srcInterval = srcInterval * 0.8 + dt * 0.2;
            }
            lastNew = n.QuadPart;
        }

        const double tick = 1.0 / (double)cfg.targetFps;
        const bool fgActive = fgOn && renderer.HasPair() && srcInterval > tick * 1.25 && srcInterval < 0.2;

        bool didRender = false;
        if (renderer.HasFrame()) {
            if (newFrame) {
                gpu.Begin();
                if (fgActive) { renderer.RenderInterp(cfg, oledOn, 0.5f); pendingReal = true; ++generated; }
                else          { renderer.RenderReal(cfg, oledOn);          pendingReal = false; }
                gpu.End();
                didRender = true;
            } else if (pendingReal) {
                gpu.Begin();
                renderer.RenderReal(cfg, oledOn);
                gpu.End();
                pendingReal = false;
                didRender = true;
            } else if (dirty) {
                gpu.Begin();
                renderer.RenderReal(cfg, oledOn);
                gpu.End();
                didRender = true;
            }
        }
        dirty = false;
        if (didRender) { renderer.Present(vsync); ++presented; }

        gpu.Poll();
        if (cfg.adaptive && ++adaptTick >= cfg.targetFps) {
            adaptTick = 0;
            const double ms = gpu.Ms();
            if (ms > 0.0) {
                const double budget = 1000.0 / (double)cfg.targetFps;
                float ns = curScale;
                if (ms > budget * 0.70) ns = curScale - 0.1f;
                else if (ms < budget * 0.35 && curScale < cfg.scale) ns = curScale + 0.1f;
                ns = Clampf(ns, cfg.minScale, cfg.scale);
                if (std::fabs(ns - curScale) > 0.01f) {
                    curScale = ns;
                    renderer.Configure(curScale);
                    gpu.ResetAverage();
                    dirty = true;
                    Log("adaptive scale -> %.2f (gpu %.2f ms, budget %.2f ms)", curScale, ms, budget);
                }
            }
        }

        pacer.Wait();

        unsigned long long now = GetTickCount64();
        if (now - statStart >= 5000) {
            double s = (double)(now - statStart) / 1000.0;
            Log("present %.1f fps | source %.1f fps | generated %.1f/s | gpu %.2f ms | scale %.2f | %s",
                presented / s, captured / s, generated / s, gpu.Ms(), curScale, PresetName(cfg.preset));
            presented = captured = generated = 0;
            statStart = now;
        }
    }

    timeEndPeriod(1);
    Log("exit");
    if (g_log) fclose(g_log);
    if (mutex) CloseHandle(mutex);
    return 0;
}
