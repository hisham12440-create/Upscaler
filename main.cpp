// =====================================================================
//  AH Upscaler - Phase 7 (Detail Engine + realistic OLED + strict frame generation)
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
    int   targetFps = 60;
    int   vsync = 1;            // -1 auto, 0 off, 1 on
    bool  oled = true;
    bool  layered = true;       // set 0 if you get a black screen
    int   quality = 1;          // 0 = lite (no denoise / clarity, lighter on the GPU), 1 = full
    bool  framegen = true;
    int   fgSearch = 12;        // motion search range in pixels (even)
    float fgStrict = 0.80f;     // 0 = lenient, 1 = very strict (fewer ghosts, more real-frame fallback)
    float fgGhost = 0.0f;       // 0 = never cross-fade uncertain areas
    float sharpness = 0.65f;
    float clarity = 0.35f;
    float gate = 0.50f;
    float overshoot = 0.03f;
    float denoise = 0.20f;
    float shadowDetail = 0.40f;
    float saturation = 1.08f;   // realistic OLED: gentle vibrance
    float skinProtect = 0.60f;  // 0..1, reduces the saturation boost on skin tones
    float blackLevel = 0.012f;
    float depth = 0.15f;
    float gamma = 1.05f;
    float contrast = 0.20f;
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
    {"monitor", "0"}, {"target_fps", "60"}, {"vsync", "1"}, {"oled", "1"}, {"layered", "1"},
    {"quality", "1"},
    {"framegen", "1"}, {"fg_search", "12"}, {"fg_strict", "0.80"}, {"fg_ghost", "0.0"},
    {"sharpness", "0.65"}, {"clarity", "0.35"}, {"gate", "0.50"}, {"overshoot", "0.03"},
    {"denoise", "0.20"}, {"shadow_detail", "0.40"},
    {"saturation", "1.08"}, {"skin_protect", "0.60"},
    {"black_level", "0.012"}, {"black_depth", "0.15"},
    {"gamma", "1.05"}, {"contrast", "0.20"}
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
    c.targetFps    = std::clamp(I("target_fps", "60"), 24, 240);
    c.vsync        = I("vsync", "1");
    c.oled         = I("oled", "1") != 0;
    c.layered      = I("layered", "1") != 0;
    c.quality      = std::clamp(I("quality", "1"), 0, 1);
    c.framegen     = I("framegen", "1") != 0;
    c.fgSearch     = std::clamp(I("fg_search", "12"), 4, 32) & ~1;
    c.fgStrict     = Clampf(F("fg_strict", "0.80"), 0.0f, 1.0f);
    c.fgGhost      = Clampf(F("fg_ghost", "0.0"), 0.0f, 1.0f);
    c.sharpness    = Clampf(F("sharpness", "0.65"), 0.0f, 1.0f);
    c.clarity      = Clampf(F("clarity", "0.35"), 0.0f, 1.0f);
    c.gate         = Clampf(F("gate", "0.50"), 0.0f, 1.0f);
    c.overshoot    = Clampf(F("overshoot", "0.03"), 0.0f, 0.2f);
    c.denoise      = Clampf(F("denoise", "0.20"), 0.0f, 1.0f);
    c.shadowDetail = Clampf(F("shadow_detail", "0.40"), 0.0f, 1.0f);
    c.saturation   = Clampf(F("saturation", "1.08"), 0.0f, 2.0f);
    c.skinProtect  = Clampf(F("skin_protect", "0.60"), 0.0f, 1.0f);
    c.blackLevel   = Clampf(F("black_level", "0.012"), 0.0f, 0.2f);
    c.depth        = Clampf(F("black_depth", "0.15"), 0.0f, 0.9f);
    c.gamma        = Clampf(F("gamma", "1.05"), 0.5f, 2.0f);
    c.contrast     = Clampf(F("contrast", "0.20"), 0.0f, 1.0f);
    Log("config: monitor=%d fps=%d vsync=%d quality=%d framegen=%d search=%d strict=%.2f sharp=%.2f clarity=%.2f oled=%d",
        c.monitor, c.targetFps, c.vsync, c.quality, (int)c.framegen, c.fgSearch, c.fgStrict,
        c.sharpness, c.clarity, (int)c.oled);
}

// =====================================================================
// 3) Shaders (HLSL)
// =====================================================================
static const char* kShaderSrc = R"HLSL(
cbuffer C : register(b0) {
    float2 srcSize; float2 dstSize;
    float sharp; float clarity; float sat; float black;
    float gam; float contrast; float oled; float depth;
    float4 extra;   // x = noise gate, y = overshoot, z = effects on(1)/bypass(0), w = strength
    float4 extra2;  // x = denoise, y = shadow detail, z = ghost blend, w = frame-gen strictness
    float4 extra3;  // x = interp time, y = block size, z = search range, w = SAD threshold
    float4 extra4;  // y = skin-tone protection
};
Texture2D tex  : register(t0);
Texture2D texB : register(t1);
Texture2D texM : register(t2);
SamplerState lin : register(s0);

struct VSOut { float4 p : SV_Position; float2 uv : TEXCOORD0; };

VSOut VS(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// ---------------- Motion estimation (block matching) ----------------
// tex = previous frame, texB = current frame.
// Output xy = displacement (UV) from current block to its match in previous frame, z = SAD.
float BlockSAD(float2 base, float2 d, in float cl[16]) {
    float s = 0;
    [unroll] for (int j = 0; j < 4; j++) {
        [unroll] for (int k = 0; k < 4; k++) {
            float2 pp = base + d + float2(k * 2 + 1, j * 2 + 1);
            float l = Luma(tex.SampleLevel(lin, (pp + 0.5) / srcSize, 0).rgb);
            s += abs(l - cl[j * 4 + k]);
        }
    }
    return s / 16.0;
}

float4 PSMotion(VSOut i) : SV_Target {
    float2 base = floor(i.p.xy) * extra3.y;
    float cl[16];
    [unroll] for (int j = 0; j < 4; j++) {
        [unroll] for (int k = 0; k < 4; k++) {
            float2 pp = base + float2(k * 2 + 1, j * 2 + 1);
            cl[j * 4 + k] = Luma(texB.SampleLevel(lin, (pp + 0.5) / srcSize, 0).rgb);
        }
    }
    float best = BlockSAD(base, float2(0, 0), cl) - 0.003;   // bias toward zero motion
    float2 bd = float2(0, 0);
    int R = (int)extra3.z;

    // coarse search, step 4
    [loop] for (int y = -R; y <= R; y += 4) {
        [loop] for (int x = -R; x <= R; x += 4) {
            float s = BlockSAD(base, float2(x, y), cl);
            if (s < best) { best = s; bd = float2(x, y); }
        }
    }
    // refine, step 2
    float2 c0 = bd;
    [loop] for (int y2 = -1; y2 <= 1; y2++) {
        [loop] for (int x2 = -1; x2 <= 1; x2++) {
            float2 d = c0 + float2(x2, y2) * 2.0;
            float s = BlockSAD(base, d, cl);
            if (s < best) { best = s; bd = d; }
        }
    }
    // refine, step 1
    c0 = bd;
    [loop] for (int y3 = -1; y3 <= 1; y3++) {
        [loop] for (int x3 = -1; x3 <= 1; x3++) {
            float2 d = c0 + float2(x3, y3);
            float s = BlockSAD(base, d, cl);
            if (s < best) { best = s; bd = d; }
        }
    }
    return float4(bd / srcSize, best, 1);
}

// ---------------- Motion-compensated interpolation (strict) ----------------
// Per pixel: test the vectors of the 3x3 neighbouring blocks, keep the best one, then fall back to
// the real current frame wherever the result is not trustworthy (low confidence, inconsistent
// neighbour vectors, forward/backward disagreement) or the pixel is static (HUD, menus).
float4 PSInterp(VSOut i) : SV_Target {
    float t = extra3.x;

    uint mwU, mhU;
    texM.GetDimensions(mwU, mhU);
    const int2 mdim = int2((int)mwU, (int)mhU);
    const int2 bc = int2(floor(i.p.xy / extra3.y));

    float3 cur = texB.SampleLevel(lin, i.uv, 0).rgb;
    float3 prv = tex.SampleLevel(lin, i.uv, 0).rgb;

    float  bestScore = 1e9;
    float  bestDis = 1.0;
    float  bestSad = 1.0;
    float2 bestD = float2(0, 0);
    float3 bestWarp = cur;

    [unroll] for (int oy = -1; oy <= 1; oy++) {
        [unroll] for (int ox = -1; ox <= 1; ox++) {
            int2 q = clamp(bc + int2(ox, oy), int2(0, 0), mdim - int2(1, 1));
            float4 m = texM.Load(int3(q, 0));
            float2 d = m.xy;
            float3 p0 = tex.SampleLevel(lin,  i.uv + d * t, 0).rgb;
            float3 c0 = texB.SampleLevel(lin, i.uv - d * (1.0 - t), 0).rgb;
            float dis = length(p0 - c0);
            float score = dis + m.z * 1.5 + ((ox != 0 || oy != 0) ? 0.01 : 0.0);
            if (score < bestScore) {
                bestScore = score;
                bestDis = dis;
                bestSad = m.z;
                bestD = d;
                bestWarp = lerp(p0, c0, t);
            }
        }
    }

    // neighbour vector consistency (in pixels): edges of moving objects are rejected
    float dev = 0.0;
    [unroll] for (int oy2 = -1; oy2 <= 1; oy2++) {
        [unroll] for (int ox2 = -1; ox2 <= 1; ox2++) {
            int2 q2 = clamp(bc + int2(ox2, oy2), int2(0, 0), mdim - int2(1, 1));
            float2 d2 = texM.Load(int3(q2, 0)).xy;
            dev = max(dev, length((d2 - bestD) * srcSize));
        }
    }
    float consistent = 1.0 - smoothstep(1.5, 5.0, dev);

    float conf = 1.0 - smoothstep(extra3.w * 0.5, extra3.w, bestSad);
    float strictness = extra2.w;
    float lo = lerp(0.12, 0.03, strictness);
    float hi = lerp(0.30, 0.10, strictness);
    float ok = conf * consistent * (1.0 - smoothstep(lo, hi, bestDis));

    float3 plain = lerp(cur, lerp(prv, cur, t), extra2.z);   // mostly the real current frame
    float3 outc = lerp(plain, bestWarp, ok);

    // static pixels (HUD, menus, unchanged areas): always the real frame
    float still = 1.0 - smoothstep(0.01, 0.04, length(prv - cur));
    outc = lerp(outc, cur, still);
    return float4(outc, 1);
}

// ---------------- Detail engine + realistic OLED look (single fused pass) ----------------
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

    // ---- edge-preserving denoise (bilateral on luma), skipped in lite mode ----
    float3 base = N[4];
    if (extra2.x > 0.001) {
        float3 acc = N[4];
        float  ws = 1.0;
        [unroll] for (int k2 = 0; k2 < 9; k2++) {
            if (k2 == 4) continue;
            float d = Y[k2] - Y[4];
            float w = exp(-d * d * kInv);
            acc += N[k2] * w;
            ws += w;
        }
        base = lerp(N[4], acc / ws, saturate(extra2.x));
    }
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
        float tt = abs(2.0 * lbase - 1.0);
        float mask = saturate(1.0 - tt * tt + 0.15);
        dl += dtl * mask * lerp(0.5, 1.0, gate);
    }

    // ---- halo limiter ----
    float lnew = clamp(lbase + dl, lmn - extra.y, lmx + extra.y);
    float3 c = saturate(base + (lnew - lbase));

    // ---- realistic OLED look ----
    if (oled > 0.5) {
        // gentle vibrance, with skin-tone protection
        float l = Luma(c);
        float chroma = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
        float skin = smoothstep(0.0, 0.08, c.r - c.g) * smoothstep(0.0, 0.08, c.g - c.b)
                   * (1.0 - smoothstep(0.30, 0.55, chroma));
        float boost = (sat - 1.0) * (1.0 - saturate(chroma)) * (1.0 - 0.8 * extra4.y * skin);
        c = saturate(lerp(l.xxx, c, 1.0 + boost));

        // true-black floor
        c = saturate((c - black) / (1.0 - black));

        // soft shadow falloff (hue preserved)
        float l2 = Luma(c);
        c *= lerp(1.0 - depth, 1.0, smoothstep(0.0, 0.35, l2));

        // shadow detail: lift dark-but-not-black pixels, true black untouched
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
// 4) GPU timer (diagnostics: how much GPU time the upscaler itself uses)
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
private:
    static const int N = 6;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<ID3D11Query> dj_[N], t0_[N], t1_[N];
    int w_ = 0, r_ = 0, inflight_ = 0;
    bool active_ = false;
    double ema_ = -1.0;
};

// =====================================================================
// 5) Screen capture (Desktop Duplication)
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
// 6) Renderer
// =====================================================================
struct alignas(16) CBData {
    float srcSize[2]; float dstSize[2];
    float sharp, clarity, sat, black;
    float gam, contrast, oled, depth;
    float extra[4];
    float extra2[4];
    float extra3[4];
    float extra4[4];
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
        MakePS("PSFinal", psFinal_);
        MakePS("PSMotion", psMotion_);
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

    // (Re)create history + motion buffers whenever the capture size/format changes
    void EnsureSource(int w, int h, DXGI_FORMAT fmt) {
        if (hist_texPrev_ && srcW_ == w && srcH_ == h && srcFmt_ == fmt) return;
        srcW_ = w; srcH_ = h; srcFmt_ = fmt;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = fmt; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hist_texPrev_.Reset(); hist_texCurr_.Reset(); prevSRV_.Reset(); currSRV_.Reset();
        AH_CHECK(dev_->CreateTexture2D(&d, nullptr, &hist_texPrev_), "Create prev frame");
        AH_CHECK(dev_->CreateTexture2D(&d, nullptr, &hist_texCurr_), "Create curr frame");
        AH_CHECK(dev_->CreateShaderResourceView(hist_texPrev_.Get(), nullptr, &prevSRV_), "prev SRV");
        AH_CHECK(dev_->CreateShaderResourceView(hist_texCurr_.Get(), nullptr, &currSRV_), "curr SRV");
        mw_ = (w + 7) / 8; mh_ = (h + 7) / 8;
        MakeTarget(motion_, mw_, mh_, DXGI_FORMAT_R16G16B16A16_FLOAT);
        MakeTarget(interp_, w, h, DXGI_FORMAT_R8G8B8A8_UNORM);
        hist_ = 0;
        Log("source buffers %dx%d, motion %dx%d", w, h, mw_, mh_);
    }

    void PushFrame(ID3D11Texture2D* captured) {
        if (hist_ == 0) ctx_->CopyResource(hist_texPrev_.Get(), captured);
        else            ctx_->CopyResource(hist_texPrev_.Get(), hist_texCurr_.Get());
        ctx_->CopyResource(hist_texCurr_.Get(), captured);
        if (hist_ < 2) ++hist_;
    }
    bool HasFrame() const { return hist_ >= 1; }
    bool HasPair() const { return hist_ >= 2; }

    void RenderReal(const Config& cfg, bool oledOn, bool fxOn, float strength) {
        Final(currSRV_.Get(), cfg, oledOn, fxOn, strength);
    }

    // Generate the in-between frame (t = 0.5) from prev and curr
    void RenderInterp(const Config& cfg, bool oledOn, bool fxOn, float strength, float t) {
        CBData cb{};
        FillCommon(cb, cfg, oledOn, fxOn, strength);
        cb.extra3[0] = t; cb.extra3[1] = 8.0f; cb.extra3[2] = (float)cfg.fgSearch; cb.extra3[3] = 0.06f;

        ID3D11ShaderResourceView* me[2] = { prevSRV_.Get(), currSRV_.Get() };
        Pass(psMotion_.Get(), me, 2, motion_.rtv.Get(), mw_, mh_, cb);

        ID3D11ShaderResourceView* ip[3] = { prevSRV_.Get(), currSRV_.Get(), motion_.srv.Get() };
        Pass(psInterp_.Get(), ip, 3, interp_.rtv.Get(), srcW_, srcH_, cb);

        Final(interp_.srv.Get(), cfg, oledOn, fxOn, strength);
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

    void FillCommon(CBData& cb, const Config& cfg, bool oledOn, bool fxOn, float strength) {
        const bool lite = (cfg.quality == 0);
        cb.srcSize[0] = (float)srcW_;  cb.srcSize[1] = (float)srcH_;
        cb.dstSize[0] = (float)dispW_; cb.dstSize[1] = (float)dispH_;
        cb.sharp = cfg.sharpness;
        cb.clarity = lite ? 0.0f : cfg.clarity;
        cb.sat = cfg.saturation;  cb.black = cfg.blackLevel;
        cb.gam = cfg.gamma;       cb.contrast = cfg.contrast;
        cb.oled = oledOn ? 1.0f : 0.0f;
        cb.depth = cfg.depth;
        cb.extra[0] = cfg.gate; cb.extra[1] = cfg.overshoot;
        cb.extra[2] = fxOn ? 1.0f : 0.0f; cb.extra[3] = strength;
        cb.extra2[0] = lite ? 0.0f : cfg.denoise;
        cb.extra2[1] = cfg.shadowDetail;
        cb.extra2[2] = cfg.fgGhost; cb.extra2[3] = cfg.fgStrict;
        cb.extra4[1] = cfg.skinProtect;
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
        ID3D11ShaderResourceView* none[3] = {nullptr, nullptr, nullptr};
        ctx_->PSSetShaderResources(0, n, none);
    }

    void Final(ID3D11ShaderResourceView* src, const Config& cfg, bool oledOn, bool fxOn, float strength) {
        CBData cb{};
        FillCommon(cb, cfg, oledOn, fxOn, strength);
        ID3D11ShaderResourceView* s[1] = { src };
        Pass(psFinal_.Get(), s, 1, bbRTV_.Get(), dispW_, dispH_, cb);
    }

    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain1> sc_;
    ComPtr<ID3D11RenderTargetView> bbRTV_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> psFinal_, psMotion_, psInterp_;
    ComPtr<ID3D11SamplerState> samp_;
    ComPtr<ID3D11Buffer> cb_;
    Target motion_, interp_;
    ComPtr<ID3D11Texture2D> hist_texPrev_, hist_texCurr_;
    ComPtr<ID3D11ShaderResourceView> prevSRV_, currSRV_;
    int dispW_ = 0, dispH_ = 0;
    int srcW_ = 0, srcH_ = 0, mw_ = 0, mh_ = 0, hist_ = 0;
    DXGI_FORMAT srcFmt_ = DXGI_FORMAT_UNKNOWN;
};

// =====================================================================
// 7) Main
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
    Log("AH Upscaler Phase 7 starting");

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

    RegisterHotKey(hwnd, 1, MOD_NOREPEAT, VK_F8);   // reload config
    RegisterHotKey(hwnd, 2, MOD_NOREPEAT, VK_F9);   // toggle OLED look
    RegisterHotKey(hwnd, 3, MOD_NOREPEAT, VK_F10);  // exit
    RegisterHotKey(hwnd, 4, MOD_NOREPEAT, VK_F11);  // pause / resume
    RegisterHotKey(hwnd, 5, MOD_NOREPEAT, VK_F7);   // toggle frame generation
    RegisterHotKey(hwnd, 6, MOD_NOREPEAT, VK_F6);   // A/B: all enhancements on/off
    RegisterHotKey(hwnd, 7, MOD_NOREPEAT, VK_F5);   // cycle enhancement strength

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
    renderer.EnsureSource(capture.Width(), capture.Height(), capture.Format());

    GpuTimer gpu;
    gpu.Init(dev.Get(), ctx.Get());

    timeBeginPeriod(1);

    LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
    auto Now = [&]() { LARGE_INTEGER n; QueryPerformanceCounter(&n); return (LONGLONG)n.QuadPart; };

    const float kStrength[3] = { 0.6f, 1.0f, 1.5f };
    int sIdx = 1;

    LONGLONG lastNew = 0, lastPresent = 0, realDue = 0;
    double srcInterval = 1.0 / (double)cfg.targetFps;   // smoothed seconds between source frames

    bool running = true, paused = false, oledOn = cfg.oled, fxOn = true, fgOn = cfg.framegen;
    bool fgState = false;          // frame generation currently active (with hysteresis)
    bool dirty = false;            // need to redraw the latest real frame
    bool pendingReal = false;      // an interpolated frame was shown; real frame is due at realDue
    unsigned long long statStart = GetTickCount64();
    int presented = 0, captured = 0, generated = 0;

    while (running) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) { running = false; break; }
            if (m.message == WM_HOTKEY) {
                switch (m.wParam) {
                case 1:
                    LoadConfig(cfg, iniPath);
                    oledOn = cfg.oled; fgOn = cfg.framegen;
                    fgState = false; pendingReal = false; dirty = true;
                    break;
                case 2: oledOn = !oledOn; dirty = true; break;
                case 3: running = false; break;
                case 4:
                    paused = !paused;
                    ShowWindow(hwnd, paused ? SW_HIDE : SW_SHOWNOACTIVATE);
                    pendingReal = false;
                    if (!paused) dirty = true;
                    break;
                case 5: fgOn = !fgOn; pendingReal = false; dirty = true; Log("frame generation %s", fgOn ? "ON" : "OFF"); break;
                case 6: fxOn = !fxOn; dirty = true; Log("enhancements %s", fxOn ? "ON" : "OFF"); break;
                case 7: sIdx = (sIdx + 1) % 3; dirty = true; Log("strength %.1f", kStrength[sIdx]); break;
                }
            }
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (!running) break;
        if (paused) { Sleep(50); continue; }

        // Wait for the next desktop frame, but wake up in time for a scheduled real frame
        UINT timeoutMs = 4;
        if (pendingReal) {
            LONGLONG left = realDue - Now();
            timeoutMs = left <= 0 ? 0u : (UINT)std::min<LONGLONG>(left * 1000 / qf.QuadPart, 50);
        }
        CapResult r = capture.Acquire(timeoutMs);
        if (r == CapResult::Lost) {
            Sleep(100);
            if (capture.Recreate())
                renderer.EnsureSource(capture.Width(), capture.Height(), capture.Format());
            pendingReal = false;
            continue;
        }

        const LONGLONG nowQ = Now();
        const bool newFrame = (r == CapResult::NewFrame);
        if (newFrame) {
            renderer.PushFrame(capture.Tex());
            ++captured;
            if (lastNew != 0) {
                double dt = (double)(nowQ - lastNew) / (double)qf.QuadPart;
                if (dt > 0.001 && dt < 0.5) srcInterval = srcInterval * 0.8 + dt * 0.2;
            }
            lastNew = nowQ;
        }

        // Frame generation only when the source is steadily around half the target rate
        // (e.g. ~30 fps source on a 60 Hz target). Hysteresis avoids flapping.
        const double tick = 1.0 / (double)cfg.targetFps;
        const bool wantFg = fgOn && renderer.HasPair()
                         && srcInterval > tick * 1.35 && srcInterval < tick * 2.6;
        if (!fgState && wantFg) fgState = true;
        else if (fgState && (!fgOn || srcInterval < tick * 1.15 || srcInterval > tick * 3.0)) fgState = false;

        const LONGLONG minGap = (LONGLONG)(tick * 0.8 * (double)qf.QuadPart);
        const bool gapOk = (nowQ - lastPresent) >= minGap;
        const float str = kStrength[sIdx];

        bool didRender = false;
        if (renderer.HasFrame()) {
            if (newFrame) {
                if (fgState) {
                    gpu.Begin();
                    renderer.RenderInterp(cfg, oledOn, fxOn, str, 0.5f);
                    gpu.End();
                    pendingReal = true;
                    realDue = nowQ + (LONGLONG)(srcInterval * 0.5 * (double)qf.QuadPart);
                    dirty = false;
                    ++generated;
                    didRender = true;
                } else if (gapOk) {
                    gpu.Begin();
                    renderer.RenderReal(cfg, oledOn, fxOn, str);
                    gpu.End();
                    pendingReal = false; dirty = false;
                    didRender = true;
                } else {
                    dirty = true;   // source faster than target: show it on the next slot
                    pendingReal = false;
                }
            } else if (pendingReal && nowQ >= realDue) {
                gpu.Begin();
                renderer.RenderReal(cfg, oledOn, fxOn, str);
                gpu.End();
                pendingReal = false; dirty = false;
                didRender = true;
            } else if (dirty && !pendingReal && gapOk) {
                gpu.Begin();
                renderer.RenderReal(cfg, oledOn, fxOn, str);
                gpu.End();
                dirty = false;
                didRender = true;
            }
        }
        if (didRender) { renderer.Present(vsync); ++presented; lastPresent = Now(); }

        gpu.Poll();

        const unsigned long long now = GetTickCount64();
        if (now - statStart >= 5000) {
            double s = (double)(now - statStart) / 1000.0;
            Log("present %.1f fps | source %.1f fps | generated %.1f/s | gpu %.2f ms | fg %d",
                presented / s, captured / s, generated / s, gpu.Ms(), (int)fgState);
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
