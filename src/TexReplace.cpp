#include "TexReplace.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <unordered_map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdint>
#include <filesystem>

namespace melonDS {

// Удобные геттеры/сеттеры (инлайн, без лишних зависимостей)
bool TexReplace_ReplaceEnabled()  { return gEnableTexReplace.load(std::memory_order_relaxed); }
bool TexReplace_DumpEnabled()     { return gEnable3DTexDump.load(std::memory_order_relaxed); }
void TexReplace_SetReplace(bool v){ gEnableTexReplace.store(v, std::memory_order_relaxed); }
void TexReplace_SetDump(bool v)   { gEnable3DTexDump.store(v, std::memory_order_relaxed); }

static std::mutex                gReplMx;
static std::unordered_map<uint64_t, std::shared_ptr<ReplacementTex>> gByHash; // key: h64^fmt^w^h (как у тебя sig)
static std::unordered_map<uint64_t, std::weak_ptr<ReplacementTex>>   gBind;   // key: vramaddr^texparam^texpal

static inline uint64_t MakeSig(uint64_t h64, uint32_t fmt, uint16_t w, uint16_t h) {
    return h64 ^ (uint64_t(fmt) << 56) ^ (uint64_t(w) << 32) ^ (uint64_t(h) << 16);
}
static inline uint64_t MakeBindKey(uint32_t vramaddr, uint32_t texparam, uint32_t texpal) {
    // простой стабильный ключ бинда для быстрого доступа на пикселях
    uint64_t k = vramaddr;
    k = (k << 32) ^ (texparam * 1469598103u) ^ (texpal * 16777619u);
    return k;
}

void ClearBindings() {
    std::lock_guard<std::mutex> lk(gReplMx);
    gBind.clear();
}

static std::shared_ptr<ReplacementTex> LoadPNGFor(uint64_t h64, uint32_t fmt, int ow, int oh)
{
    char fname[512];
    std::snprintf(fname, sizeof(fname), "text_replace/mod/%016llX_fmt%u_%dx%d.png",
                  (unsigned long long)h64, fmt, ow, oh);

    int W=0, H=0, C=0;
    stbi_uc* data = stbi_load(fname, &W, &H, &C, 4);
    if (!data) return nullptr;

    auto R = std::make_shared<ReplacementTex>();
    R->w = W; R->h = H;
    R->sx = (float)W / (float)ow;
    R->sy = (float)H / (float)oh;
    R->rgba.assign(data, data + (W*H*4));
    stbi_image_free(data);
    return R;
}

// Публичные функции (зови из рендера)
std::shared_ptr<ReplacementTex> FindOrLoadByHash(uint64_t h64, uint32_t fmt, int ow, int oh)
{
    std::lock_guard<std::mutex> lk(gReplMx);
    uint64_t sig = MakeSig(h64, fmt, (uint16_t)ow, (uint16_t)oh);
    if (auto it = gByHash.find(sig); it != gByHash.end()) return it->second;

    auto R = LoadPNGFor(h64, fmt, ow, oh);
    if (R) gByHash.emplace(sig, R);
    return R;
}

std::shared_ptr<ReplacementTex> GetBound(uint32_t vramaddr, uint32_t texparam, uint32_t texpal)
{
    std::lock_guard<std::mutex> lk(gReplMx);
    uint64_t bk = MakeBindKey(vramaddr, texparam, texpal);
    auto it = gBind.find(bk);
    if (it == gBind.end()) return nullptr;
    return it->second.lock();
}

void BindReplacement(uint32_t vramaddr, uint32_t texparam, uint32_t texpal,
                     const std::shared_ptr<ReplacementTex>& R)
{
    if (!R) return;
    std::lock_guard<std::mutex> lk(gReplMx);
    uint64_t bk = MakeBindKey(vramaddr, texparam, texpal);
    gBind[bk] = R;
}

void ClearAllReplacements()    // вызови при загрузке ROM/Reset
{
    std::lock_guard<std::mutex> lk(gReplMx);
    gByHash.clear();
    gBind.clear();
}

} // namespace melonDS
