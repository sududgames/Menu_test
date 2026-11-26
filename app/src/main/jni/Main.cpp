#include <jni.h>
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <ctime>
#include <thread>
#include <chrono>
#include <dlfcn.h>
#include <android/log.h>

#include "Includes/obfuscate.h"
#include "Includes/Utils.hpp"
#include "Menu/Menu.hpp"

#ifndef ANDROID_LOG_INFO
#define ANDROID_LOG_INFO 4
#endif

// ===================== Лог: logcat + файл =====================
// Файл: /storage/emulated/0/texmod_log.txt (в корне хранилища, без папок).
// До выдачи доступа сообщения копятся в памяти и сбрасываются в файл,
// как только доступ появится.

static std::string g_logPath = "/storage/emulated/0/texmod_log.txt";
static FILE* g_logFile = nullptr;
static std::vector<std::string> g_logPending;

static void LogMsg(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    char ts[16];
    time_t t = time(nullptr);
    struct tm lt;
    localtime_r(&t, &lt);
    strftime(ts, sizeof(ts), "%H:%M:%S", &lt);

    __android_log_print(ANDROID_LOG_INFO, "TEXMOD", "%s", buf);

    std::string line = std::string("[") + ts + "] " + buf;
    if (g_logFile) {
        fprintf(g_logFile, "%s\n", line.c_str());
        fflush(g_logFile);
    } else {
        g_logPending.push_back(line);
    }
}

static void OpenLogFile() {
    if (g_logFile) return;
    g_logFile = fopen(g_logPath.c_str(), "a");
    if (g_logFile) {
        char ts[24];
        time_t t = time(nullptr);
        struct tm lt;
        localtime_r(&t, &lt);
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &lt);
        fprintf(g_logFile, "\n===== launch %s =====\n", ts);
        for (size_t i = 0; i < g_logPending.size(); ++i)
            fprintf(g_logFile, "%s\n", g_logPending[i].c_str());
        fflush(g_logFile);
        g_logPending.clear();
    }
}

// ===================== il2cpp API =====================
typedef void*  (*il2cpp_domain_get_t)();
typedef void** (*il2cpp_domain_get_assemblies_t)(void*, size_t*);
typedef void*  (*il2cpp_assembly_get_image_t)(void*);
typedef void*  (*il2cpp_class_from_name_t)(void*, const char*, const char*);
typedef void*  (*il2cpp_class_get_method_from_name_t)(void*, const char*, int);
typedef void*  (*il2cpp_runtime_invoke_t)(void*, void*, void**, void**);
typedef void*  (*il2cpp_thread_attach_t)(void*);
typedef const char* (*il2cpp_string_chars_t)(void*);
typedef uint32_t    (*il2cpp_array_length_t)(void*);
typedef void*  (*il2cpp_array_new_t)(void*, uintptr_t);
typedef void*  (*il2cpp_class_get_type_t)(void*);
typedef void*  (*il2cpp_type_get_object_t)(void*);
typedef void*  (*il2cpp_value_box_t)(void*, void*);

static il2cpp_domain_get_t il2cpp_domain_get = nullptr;
static il2cpp_domain_get_assemblies_t il2cpp_domain_get_assemblies = nullptr;
static il2cpp_assembly_get_image_t il2cpp_assembly_get_image = nullptr;
static il2cpp_class_from_name_t il2cpp_class_from_name = nullptr;
static il2cpp_class_get_method_from_name_t il2cpp_class_get_method_from_name = nullptr;
static il2cpp_runtime_invoke_t il2cpp_runtime_invoke = nullptr;
static il2cpp_thread_attach_t il2cpp_thread_attach = nullptr;
static il2cpp_string_chars_t il2cpp_string_chars = nullptr;
static il2cpp_array_length_t il2cpp_array_length = nullptr;
static il2cpp_array_new_t il2cpp_array_new = nullptr;
static il2cpp_class_get_type_t il2cpp_class_get_type = nullptr;
static il2cpp_type_get_object_t il2cpp_type_get_object = nullptr;
static il2cpp_value_box_t il2cpp_value_box = nullptr;

// Классы
static void* texKlass   = nullptr; // UnityEngine.Texture2D
static void* resKlass   = nullptr; // UnityEngine.Resources
static void* byteKlass  = nullptr; // System.Byte
static void* boolKlass  = nullptr; // System.Boolean
static void* objKlass   = nullptr; // UnityEngine.Object
static void* imgConvKlass = nullptr; // UnityEngine.ImageConversion

// Методы
static void* mi_findAll     = nullptr; // Resources.FindObjectsOfTypeAll(Type)
static void* mi_objGetName  = nullptr; // Object.get_name()
static void* mi_encodePNG   = nullptr; // EncodeToPNG (стат., ImageConversion) или null
static void* mi_loadImage   = nullptr; // LoadImage (стат., ImageConversion) или null
static void* mi_apply       = nullptr; // Texture2D.Apply(bool,bool)
static void* mi_isReadable  = nullptr; // Texture2D.get_isReadable()
static void* mi_getWidth    = nullptr; // Texture2D.get_width()
static void* mi_getHeight   = nullptr; // Texture2D.get_height()
static void* mi_getPixels32 = nullptr; // Texture2D.GetPixels32()

static void* texType   = nullptr; // System.Type для Texture2D
static void* g_domain  = nullptr;

static bool g_pngStatic = false; // EncodeToPNG/LoadImage найдены статическими

static std::string g_dumpDir;
static std::string g_modDir;

static const char* kStorageRoot = "/storage/emulated/0";
// Ограничение на дамп через пиксели: не более 16 млн пикселей (4096x4096)
static const uint64_t kMaxPixels = 16ull * 1024ull * 1024ull;

// Смещения il2cpp (arm64)
static constexpr uintptr_t kArrayHeaderSize = 0x20;   // klass, monitor, bounds, max_length
static constexpr uintptr_t kBoxedValueOffset = 0x10;  // klass, monitor, value

static inline void* ArrayData(void* arr) {
    return (void*)((char*)arr + kArrayHeaderSize);
}

// ===================== Поток: аттач перед работой =====================
// Аттач вынесен из ResolveIl2Cpp: на повторных запусках игры домен мог
// вернуться до полной инициализации рантайма, и аттач ронял весь процесс.
// Аттач идемпотентный — делаем его перед каждым блоком работы с il2cpp.
static bool EnsureThread() {
    if (!g_domain || !il2cpp_thread_attach) return false;
    il2cpp_thread_attach(g_domain);
    return true;
}

// ===================== Доступ к хранилищу =====================
static bool StorageAccessOK() {
    std::string p = std::string(kStorageRoot) + "/texdump_access_test";
    FILE* t = fopen(p.c_str(), "wb");
    if (!t) return false;
    fclose(t);
    remove(p.c_str());
    return true;
}

static bool DumpExists() {
    DIR* d = opendir(g_dumpDir.c_str());
    if (!d) return false;
    bool has = false;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        // texmod_log.txt не считаем содержимым дампа
        if (e->d_name[0] != '.' && strcmp(e->d_name, "texmod_log.txt") != 0) {
            has = true;
            break;
        }
    }
    closedir(d);
    return has;
}

// ===================== PNG-кодировщик (без zlib: stored-блоки) =====================
static uint32_t Crc32(const uint8_t* data, size_t len) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
        c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static uint32_t Adler32(const uint8_t* data, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

static void Put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24));
    v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)(x & 0xFF));
}

static void Chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& payload) {
    Put32(png, (uint32_t)payload.size());
    for (int i = 0; i < 4; ++i) png.push_back((uint8_t)type[i]);
    if (!payload.empty()) {
        png.insert(png.end(), payload.begin(), payload.end());
        size_t crcStart = png.size() - payload.size() - 4;
        uint32_t crc = Crc32(&png[crcStart], payload.size() + 4);
        Put32(png, crc);
    } else {
        uint32_t crc = Crc32(&png[png.size() - 4], 4);
        Put32(png, crc);
    }
}

// RGBA32 -> PNG (zlib stored-блоки, без сжатия — файл больше, но читается везде)
static bool WritePngRGBA(const std::string& path, uint32_t w, uint32_t h, const uint8_t* px) {
    std::vector<uint8_t> raw;
    raw.reserve((size_t)h * (1 + (size_t)w * 4));
    for (uint32_t y = 0; y < h; ++y) {
        raw.push_back(0); // filter: none
        const uint8_t* line = px + (size_t)y * w * 4;
        raw.insert(raw.end(), line, line + (size_t)w * 4);
    }

    std::vector<uint8_t> z;
    z.push_back(0x78);
    z.push_back(0x01);
    size_t off = 0;
    while (off < raw.size()) {
        size_t n = raw.size() - off;
        if (n > 65535) n = 65535;
        bool last = (off + n == raw.size());
        z.push_back(last ? 1 : 0);
        z.push_back((uint8_t)(n & 0xFF));
        z.push_back((uint8_t)((n >> 8) & 0xFF));
        z.push_back((uint8_t)((~n) & 0xFF));
        z.push_back((uint8_t)(((~n) >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
        off += n;
    }
    Put32(z, Adler32(raw.data(), raw.size()));

    std::vector<uint8_t> png;
    const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    png.insert(png.end(), sig, sig + 8);

    std::vector<uint8_t> ihdr;
    Put32(ihdr, w);
    Put32(ihdr, h);
    ihdr.push_back(8); // bit depth
    ihdr.push_back(6); // color type: RGBA
    ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    Chunk(png, "IHDR", ihdr);
    Chunk(png, "IDAT", z);
    Chunk(png, "IEND", std::vector<uint8_t>());

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t wr = fwrite(png.data(), 1, png.size(), f);
    fclose(f);
    return wr == png.size();
}

// ===================== Утилиты =====================
static std::string SanitizeName(const char* name) {
    std::string out;
    for (const char* p = name; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '_' || c == '-') out += (char)c;
        else out += '_';
    }
    if (out.empty()) out = "unnamed";
    return out;
}

static bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return false; }
    out.resize((size_t)sz);
    size_t r = fread(out.data(), 1, (size_t)sz, f);
    fclose(f);
    return r == (size_t)sz;
}

// ===================== Резолв il2cpp =====================
static void ResolveIl2Cpp() {
    void* h = nullptr;
    while (!h) {
        h = dlopen(OBFUSCATE("libil2cpp.so"), RTLD_NOW | RTLD_GLOBAL);
        if (!h) {
            LogMsg("waiting for libil2cpp.so...");
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }
    LogMsg("libil2cpp.so loaded");

    il2cpp_domain_get              = (il2cpp_domain_get_t)             dlsym(h, OBFUSCATE("il2cpp_domain_get"));
    il2cpp_domain_get_assemblies   = (il2cpp_domain_get_assemblies_t)  dlsym(h, OBFUSCATE("il2cpp_domain_get_assemblies"));
    il2cpp_assembly_get_image      = (il2cpp_assembly_get_image_t)     dlsym(h, OBFUSCATE("il2cpp_assembly_get_image"));
    il2cpp_class_from_name         = (il2cpp_class_from_name_t)        dlsym(h, OBFUSCATE("il2cpp_class_from_name"));
    il2cpp_class_get_method_from_name = (il2cpp_class_get_method_from_name_t) dlsym(h, OBFUSCATE("il2cpp_class_get_method_from_name"));
    il2cpp_runtime_invoke          = (il2cpp_runtime_invoke_t)         dlsym(h, OBFUSCATE("il2cpp_runtime_invoke"));
    il2cpp_thread_attach           = (il2cpp_thread_attach_t)          dlsym(h, OBFUSCATE("il2cpp_thread_attach"));
    il2cpp_string_chars            = (il2cpp_string_chars_t)           dlsym(h, OBFUSCATE("il2cpp_string_chars"));
    il2cpp_array_length            = (il2cpp_array_length_t)           dlsym(h, OBFUSCATE("il2cpp_array_length"));
    il2cpp_array_new               = (il2cpp_array_new_t)              dlsym(h, OBFUSCATE("il2cpp_array_new"));
    il2cpp_class_get_type          = (il2cpp_class_get_type_t)         dlsym(h, OBFUSCATE("il2cpp_class_get_type"));
    il2cpp_type_get_object         = (il2cpp_type_get_object_t)        dlsym(h, OBFUSCATE("il2cpp_type_get_object"));
    il2cpp_value_box               = (il2cpp_value_box_t)              dlsym(h, OBFUSCATE("il2cpp_value_box"));

    if (!il2cpp_domain_get || !il2cpp_runtime_invoke || !il2cpp_domain_get_assemblies) {
        LogMsg("core il2cpp symbols missing, thread exit");
        return;
    }

    // Ждём домен. БЕЗ аттача здесь — только чтение указателя.
    while (!g_domain) {
        g_domain = il2cpp_domain_get();
        if (!g_domain) {
            LogMsg("waiting for il2cpp domain...");
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }
    LogMsg("il2cpp domain ready");

    // Ждём непустой список сборок — рантайм ещё может быть не готов
    size_t count = 0;
    void** assemblies = nullptr;
    for (int tries = 0; tries < 30; ++tries) {
        assemblies = il2cpp_domain_get_assemblies(g_domain, &count);
        if (assemblies && count > 0) break;
        LogMsg("waiting for assemblies...");
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    if (!assemblies || count == 0) {
        LogMsg("assemblies never appeared, thread exit");
        return;
    }

    // Обход сборок: логируем имена (Il2CppImage::name — первое поле образа),
    // резолвим классы
    for (size_t i = 0; i < count; ++i) {
        void* image = il2cpp_assembly_get_image(assemblies[i]);
        if (!image) continue;
        const char* imgName = *(const char**)image;
        LogMsg("asm[%zu]: %s", i, imgName ? imgName : "?");

        if (!texKlass)     texKlass     = il2cpp_class_from_name(image, OBFUSCATE("UnityEngine"), OBFUSCATE("Texture2D"));
        if (!resKlass)     resKlass     = il2cpp_class_from_name(image, OBFUSCATE("UnityEngine"), OBFUSCATE("Resources"));
        if (!byteKlass)    byteKlass    = il2cpp_class_from_name(image, OBFUSCATE("System"), OBFUSCATE("Byte"));
        if (!boolKlass)    boolKlass    = il2cpp_class_from_name(image, OBFUSCATE("System"), OBFUSCATE("Boolean"));
        if (!objKlass)     objKlass     = il2cpp_class_from_name(image, OBFUSCATE("UnityEngine"), OBFUSCATE("Object"));
        if (!imgConvKlass) imgConvKlass = il2cpp_class_from_name(image, OBFUSCATE("UnityEngine"), OBFUSCATE("ImageConversion"));
    }

    if (resKlass) mi_findAll = il2cpp_class_get_method_from_name(resKlass, OBFUSCATE("FindObjectsOfTypeAll"), 1);
    if (objKlass) mi_objGetName = il2cpp_class_get_method_from_name(objKlass, OBFUSCATE("get_name"), 0);
    if (texKlass) {
        mi_apply       = il2cpp_class_get_method_from_name(texKlass, OBFUSCATE("Apply"), 2);
        mi_isReadable  = il2cpp_class_get_method_from_name(texKlass, OBFUSCATE("get_isReadable"), 0);
        mi_getWidth    = il2cpp_class_get_method_from_name(texKlass, OBFUSCATE("get_width"), 0);
        mi_getHeight   = il2cpp_class_get_method_from_name(texKlass, OBFUSCATE("get_height"), 0);
        mi_getPixels32 = il2cpp_class_get_method_from_name(texKlass, OBFUSCATE("GetPixels32"), 0);
        if (il2cpp_class_get_type && il2cpp_type_get_object)
            texType = il2cpp_type_get_object(il2cpp_class_get_type(texKlass));
    }
    if (imgConvKlass) {
        mi_encodePNG = il2cpp_class_get_method_from_name(imgConvKlass, OBFUSCATE("EncodeToPNG"), 1);
        mi_loadImage = il2cpp_class_get_method_from_name(imgConvKlass, OBFUSCATE("LoadImage"), 2);
        g_pngStatic = (mi_encodePNG != nullptr);
    }
    // Если ImageConversion не нашёлся — остаёмся без PNG-методов:
    // дамп пойдёт через GetPixels32 + собственный PNG-кодировщик,
    // применение PNG будет отключено до выяснения.

    LogMsg("il2cpp resolved: tex=%p imgConv=%p type=%p findAll=%p enc=%p load=%p apply=%p px32=%p",
           texKlass, imgConvKlass, texType, mi_findAll,
           mi_encodePNG, mi_loadImage, mi_apply, mi_getPixels32);
    if (!imgConvKlass)
        LogMsg("ImageConversion NOT found in %zu assemblies — pixel fallback active", count);
    if (!texType)
        LogMsg("texType missing — FindObjectsOfTypeAll unusable");
}

// ===================== Дамп =====================
static void DumpTextures() {
    if (!EnsureThread()) { LogMsg("dump: thread attach failed"); return; }
    if (!mi_findAll || !texType || !mi_objGetName) {
        LogMsg("dump skipped: missing core methods");
        return;
    }

    void* params[1] = { texType };
    void* exc = nullptr;
    void* arr = il2cpp_runtime_invoke(mi_findAll, nullptr, params, &exc);
    if (exc || !arr) {
        LogMsg("FindObjectsOfTypeAll failed");
        return;
    }

    uint32_t count = il2cpp_array_length(arr);
    LogMsg("objects found: %u", count);

    void** data = (void**)ArrayData(arr);
    int saved = 0, skipped = 0, unreadable = 0;

    for (uint32_t i = 0; i < count; ++i) {
        void* tex = data[i];
        if (!tex) { skipped++; continue; }

        exc = nullptr;
        void* nameObj = il2cpp_runtime_invoke(mi_objGetName, tex, nullptr, &exc);
        if (exc || !nameObj) { skipped++; continue; }
        const char* cname = il2cpp_string_chars(nameObj);
        if (!cname || !*cname) { skipped++; continue; }

        if (mi_isReadable) {
            exc = nullptr;
            void* r = il2cpp_runtime_invoke(mi_isReadable, tex, nullptr, &exc);
            if (!exc && r && *(bool*)((char*)r + kBoxedValueOffset) == false) {
                unreadable++;
                continue;
            }
        }

        std::string fname = SanitizeName(cname) + ".png";
        std::string path = g_dumpDir + "/" + fname;
        bool ok = false;

        if (mi_encodePNG) {
            // Unity 5.6+: статический EncodeToPNG(tex) в ImageConversion
            void* encParams[1] = { tex };
            exc = nullptr;
            void* png = g_pngStatic
                ? il2cpp_runtime_invoke(mi_encodePNG, nullptr, encParams, &exc)
                : il2cpp_runtime_invoke(mi_encodePNG, tex, nullptr, &exc);
            if (!exc && png) {
                uint32_t pngSize = il2cpp_array_length(png);
                if (pngSize > 0) {
                    FILE* f = fopen(path.c_str(), "wb");
                    if (f) {
                        size_t w = fwrite(ArrayData(png), 1, pngSize, f);
                        fclose(f);
                        ok = (w == pngSize);
                    }
                }
            }
        } else if (mi_getPixels32 && mi_getWidth && mi_getHeight) {
            // Фолбэк: пиксели + собственный PNG-кодировщик
            exc = nullptr;
            void* wObj = il2cpp_runtime_invoke(mi_getWidth, tex, nullptr, &exc);
            if (exc || !wObj) { skipped++; continue; }
            exc = nullptr;
            void* hObj = il2cpp_runtime_invoke(mi_getHeight, tex, nullptr, &exc);
            if (exc || !hObj) { skipped++; continue; }
            uint32_t w = (uint32_t)*(int32_t*)((char*)wObj + kBoxedValueOffset);
            uint32_t h = (uint32_t)*(int32_t*)((char*)hObj + kBoxedValueOffset);
            if (w == 0 || h == 0) { skipped++; continue; }
            if ((uint64_t)w * h > kMaxPixels) {
                LogMsg("skip too large: %s (%ux%u)", cname, w, h);
                skipped++;
                continue;
            }

            exc = nullptr;
            void* pixArr = il2cpp_runtime_invoke(mi_getPixels32, tex, nullptr, &exc);
            if (exc || !pixArr) { skipped++; continue; }
            uint32_t n = il2cpp_array_length(pixArr);
            if (n != w * h) { skipped++; continue; }

            ok = WritePngRGBA(path, w, h, (const uint8_t*)ArrayData(pixArr));
        }

        if (ok) saved++;
        else skipped++;
    }

    LogMsg("dump done: saved=%d skipped=%d unreadable=%d", saved, skipped, unreadable);
}

// ===================== Применение текстур =====================
static void ApplyCustomTextures() {
    if (!EnsureThread()) return;
    if (!mi_findAll || !texType || !mi_objGetName || !mi_apply || !boolKlass) return;
    if (!mi_loadImage || !byteKlass || !il2cpp_value_box || !il2cpp_array_new) {
        // LoadImage недоступен (ImageConversion не найден) — применение PNG пока нельзя
        static bool warned = false;
        if (!warned) {
            LogMsg("apply disabled: LoadImage unavailable (ImageConversion missing)");
            warned = true;
        }
        return;
    }

    void* params[1] = { texType };
    void* exc = nullptr;
    void* arr = il2cpp_runtime_invoke(mi_findAll, nullptr, params, &exc);
    if (exc || !arr) return;
    uint32_t count = il2cpp_array_length(arr);
    void** data = (void**)ArrayData(arr);

    DIR* d = opendir(g_modDir.c_str());
    if (!d) return;

    int applied = 0;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        std::string fname = e->d_name;
        if (fname.size() < 5) continue;
        if (fname.compare(fname.size() - 4, 4, ".png") != 0) continue;
        std::string want = fname.substr(0, fname.size() - 4);

        void* target = nullptr;
        for (uint32_t i = 0; i < count; ++i) {
            void* tex = data[i];
            if (!tex) continue;
            exc = nullptr;
            void* nameObj = il2cpp_runtime_invoke(mi_objGetName, tex, nullptr, &exc);
            if (exc || !nameObj) continue;
            const char* cname = il2cpp_string_chars(nameObj);
            if (cname && want == SanitizeName(cname)) { target = tex; break; }
        }
        if (!target) continue;

        std::vector<uint8_t> png;
        if (!ReadFileBytes(g_modDir + "/" + fname, png)) continue;

        void* byteArr = il2cpp_array_new(byteKlass, (uintptr_t)png.size());
        if (!byteArr) continue;
        memcpy(ArrayData(byteArr), png.data(), png.size());

        // Unity 5.6+: статический LoadImage(tex, byte[])
        void* liParams[2] = { target, byteArr };
        exc = nullptr;
        if (g_pngStatic)
            il2cpp_runtime_invoke(mi_loadImage, nullptr, liParams, &exc);
        else
            il2cpp_runtime_invoke(mi_loadImage, target, (void**)&byteArr, &exc);
        if (exc) continue;

        bool a = true, b = false;
        void* applyParams[2] = {
            il2cpp_value_box(boolKlass, &a),
            il2cpp_value_box(boolKlass, &b)
        };
        exc = nullptr;
        il2cpp_runtime_invoke(mi_apply, target, applyParams, &exc);
        if (exc) continue;

        applied++;
    }
    closedir(d);

    if (applied > 0) LogMsg("apply done: %d", applied);
}

// ===================== Главный поток =====================
static void hack_thread() {
    // libil2cpp.so загружается не сразу — ждём
    while (!isLibraryLoaded(targetLibName)) {
        sleep(1);
    }

    ResolveIl2Cpp();

    if (!texKlass || !il2cpp_runtime_invoke) {
        LogMsg("il2cpp not available, thread exit");
        return;
    }

    g_dumpDir = std::string(kStorageRoot) + "/texdump";
    g_modDir  = std::string(kStorageRoot) + "/texmod";

    // Ждём выдачи MANAGE_EXTERNAL_STORAGE — тумблер могут включить после старта
    while (!StorageAccessOK()) {
        LogMsg("storage access DENIED — enable All files access (retry 10s)");
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
    LogMsg("storage access OK");
    OpenLogFile(); // сбрасываем накопленные до этого строки в файл

    mkdir(g_dumpDir.c_str(), 0777);
    mkdir(g_modDir.c_str(), 0777);

    // Дамп с повторами: текстуры появляются после загрузки сцен
    if (!DumpExists()) {
        bool dumped = false;
        for (int attempt = 1; attempt <= 12 && !dumped; ++attempt) {
            LogMsg("dump attempt %d/12", attempt);
            DumpTextures();
            dumped = DumpExists();
            if (!dumped)
                std::this_thread::sleep_for(std::chrono::seconds(30));
        }
    } else {
        LogMsg("dump exists, skip");
    }

    // Цикл применения: подхватываем PNG из /texmod раз в минуту
    while (true) {
        ApplyCustomTextures();
        std::this_thread::sleep_for(std::chrono::seconds(60));
    }
}

// ============================================================================
// Контракт LGL: определения совпадают с Menu.hpp один-в-один.
// Setup.cpp регистрирует их через RegisterNatives.
// ============================================================================

jobjectArray GetFeatureList(JNIEnv *env, jobject context) {
    (void)context;
    jclass strClass = env->FindClass(OBFUSCATE("java/lang/String"));
    if (!strClass) return nullptr;
    return env->NewObjectArray(0, strClass, nullptr);
}

void Changes(JNIEnv *env, jclass clazz, jobject obj,
             jint featNum, jstring featName, jint value, jlong Lvalue,
             jboolean boolean, jstring str) {
    (void)env; (void)clazz; (void)obj; (void)featNum;
    (void)featName; (void)value; (void)Lvalue; (void)boolean; (void)str;
}

// ============================================================================
// Точка входа — конструктор библиотеки: запускается автоматически при
// загрузке .so в процесс игры. JNI_OnLoad находится в Setup.cpp.
// ============================================================================

__attribute__((constructor))
static void lib_main() {
    std::thread(hack_thread).detach();
}

