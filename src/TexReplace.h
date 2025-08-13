#pragma once
#include <memory>
#include <cstdint>
#include <vector>
#include <unordered_set>
#include <mutex>
#include <cstdint>

#if __cplusplus >= 201703L
  #include <filesystem>
#else
  #include <sys/stat.h>
  #include <sys/types.h>
  #ifdef _WIN32
    #include <direct.h>
  #endif
#endif


namespace melonDS {

// --- Флаги (одно определение в TexReplace.cpp)
extern std::atomic<bool> gEnableTexReplace; // Restore/Replace
extern std::atomic<bool> gEnable3DTexDump;  // Dump

// Удобные геттеры/сеттеры (инлайн, без лишних зависимостей)
inline bool TexReplace_ReplaceEnabled()  { return gEnableTexReplace.load(std::memory_order_relaxed); }
inline bool TexReplace_DumpEnabled()     { return gEnable3DTexDump.load(std::memory_order_relaxed); }
inline void TexReplace_SetReplace(bool v){ gEnableTexReplace.store(v, std::memory_order_relaxed); }
inline void TexReplace_SetDump(bool v)   { gEnable3DTexDump.store(v, std::memory_order_relaxed); }

static std::once_flag gDumpDirOnce;

// Набор уже дампнутых текстур (уникальность по содержимому)
static std::unordered_set<uint64_t> gSeen3DTex;  // ключ = комбинированный хэш

// FNV-1a 64, только верхняя-левая четверть RGBA-изображения
static inline uint64_t fnv1a64_quarterTL_rgba(const uint8_t* rgba,
                                              int width, int height,
                                              int strideBytes = 0) // по умолчанию = width*4
{
    if (!rgba || width <= 0 || height <= 0) return 0;

    const int bpp = 4;                          // RGBA
    if (strideBytes == 0) strideBytes = width * bpp;

    // если слишком маленькое изображение — хэшируем всё целиком
    const int qW = width;
    const int qH = (height >= 2) ? (height / 2) : height;

    uint64_t h = 1469598103934665603ULL;        // offset basis
    const uint64_t FNV_PRIME = 1099511628211ULL;

    for (int y = 0; y < qH; ++y) {
        const uint8_t* row = rgba + y * strideBytes;
        for (int x = 0; x < qW; ++x) {
            const uint8_t* px = row + x * bpp;
            // FNV-1a — по байтам
            h ^= px[0]; h *= FNV_PRIME;
            h ^= px[1]; h *= FNV_PRIME;
            h ^= px[2]; h *= FNV_PRIME;
            h ^= px[3]; h *= FNV_PRIME;
        }
    }
    return h;
}

static inline void EnsureDump3DDir()
{
    std::call_once(gDumpDirOnce, []{
    #if __cplusplus >= 201703L
        try {
            std::filesystem::create_directories("text_replace/dump");
        } catch (...) {
            // ignore
        }
    #else
      #ifdef _WIN32
        _mkdir("dump");
        _mkdir("dump\\3d");
      #else
        mkdir("dump", 0755);
        mkdir("dump/3d", 0755);
      #endif
    #endif
    });
}


struct ReplacementTex {
    int w = 0, h = 0;
    float sx = 1.0f, sy = 1.0f;
    std::vector<uint8_t> rgba;
    mutable uint32_t gltex = 0;
};

void ClearBindings(); 
std::shared_ptr<ReplacementTex> FindOrLoadByHash(uint64_t h64, uint32_t fmt, int ow, int oh);
std::shared_ptr<ReplacementTex> GetBound(uint32_t vramaddr, uint32_t texparam, uint32_t texpal);
void BindReplacement(uint32_t vramaddr, uint32_t texparam, uint32_t texpal,
                     const std::shared_ptr<ReplacementTex>& R);
void ClearAllReplacements();
}
