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

#define targetLibName OBFUSCATE("libil2cpp.so")

// --- лог: logcat (TAG TEXMOD) + файл /storage/emulated/0/texdump/texmod_log.txt ---
static std::string g_logPath;

static void TexLog(const char* fmt, ...) {
    char msgBuf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);
    va_end(args);

    __android_log_print(ANDROID_LOG_INFO, "TEXMOD", "%s", msgBuf);

    if (!g_logPath.empty()) {
        FILE* lf = fopen(g_logPath.c_str(), "a");
        if (lf) {
            time_t now = time(nullptr);
            char tsBuf[32];
            strftime(tsBuf, sizeof(tsBuf), "%H:%M:%S", localtime(&now));
            fprintf(lf, "[%s] %s\n", tsBuf, msgBuf);
            fclose(lf);
        }
    }
}
#define LOGI(...) TexLog(__VA_ARGS__)

// --- il2cpp API ---
typedef void*  (*il2cpp_domain_get_t)();
typedef void** (*il2cpp_domain_get_assemblies_t)(void*, size_t*);
typedef void*  (*il2cpp_assembly_get_image_t)(void*);
typedef void*  (*il2cpp_class_from_name_t)(void*, const char*, const char*);
typedef void*  (*il2cpp_class_get_method_from_name_t)(void*, const char*, int);
typedef void*  (*il2cpp_runtime_invoke_t)(void*, void*, void**, void**);
typedef void*  (*il2cpp_thread_attach_t)(void*);
typedef const char* (*il2cpp_string_chars_t)(void*);
typedef uint32_t    (*il2cpp_array_length_t)(void*);
typedef void*  (*il2cpp_array_new_t)(void*, uintptr_t);   // (klass, length)
typedef void*  (*il2cpp_class_get_type_t)(void*);
typedef void*  (*il2cpp_type_get_object_t)(void*);
typedef void*  (*il2cpp_value_box_t)(void*, void*);       // (klass, void* value) -> boxed

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

static void* texKlass   = nullptr; // UnityEngine.Texture2D (Il2CppClass*)
static void* texType    = nullptr; // System.Type для Texture2D (Il2CppObject*)
static void* resKlass   = nullptr; // UnityEngine.Resources
static void* byteKlass  = nullptr; // System.Byte
static void* boolKlass  = nullptr; // System.Boolean
static void* objKlass   = nullptr; // UnityEngine.Object
static void* imgConvKlass = nullptr; // UnityEngine.ImageConversion (Unity 5.6+)

static void* mi_findAll    = nullptr; // Resources.FindObjectsOfTypeAll(Type)
static void* mi_objGetName = nullptr; // Object.get_name()
static void* mi_encodePNG  = nullptr; // EncodeToPNG (статич. из ImageConversion или инстансный)
static void* mi_loadImage  = nullptr; // LoadImage (статич. из ImageConversion или инстансный)
static void* mi_apply      = nullptr; // Texture2D.Apply(bool, bool)
static void* mi_isReadable = nullptr; // Texture2D.get_isReadable()

static bool g_texConvStatic = false; // true — методы из ImageConversion (статические)

static std::string g_dumpDir;
static std::string g_modDir;

static const char* kStorageRoot = "/storage/emulated/0";

// arm64 il2cpp-массив: klass(8) + monitor(8) + bounds(8) + max_length(8) = 0x20
static constexpr uintptr_t kArrayHeaderSize = 0x20;
static inline void* ArrayData(void* arr) { return (void*)((char*)arr + kArrayHeaderSize); }

// Il2CppObject { klass(8) + monitor(8) + value[0] } -> значение с 0x10
static constexpr uintptr_t kBoxedValueOffset = 0x10;

static bool StorageAccessOK() {
    std::string probe = std::string(kStorageRoot) + "/texdump_access_test";
    FILE* t = fopen(probe.c_str(), "wb");
    if (!t) return false;
    fclose(t);
    remove(probe.c_str());
    return true;
}

static bool DumpExists() {
    DIR* d = opendir(g_dumpDir.c_str());
    if (!d) return false;
    bool has = false;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] != '.') { has = true; break; }
    }
    closedir(d);
    return has;
}

static void ResolveIl2Cpp() {
    // Страховка: библиотека уже должна быть загружена (isLibraryLoaded),
    // но на всякий случай ждём и через dlopen
    void* h = nullptr;
    while (!h) {
        h = dlopen(OBFUSCATE("libil2cpp.so"), RTLD_NOW | RTLD_GLOBAL);
        if (!h) {
            LOGI(OBFUSCATE("waiting for libil2cpp.so..."));
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }
    LOGI(OBFUSCATE("libil2cpp.so loaded"));

    il2cpp_domain_get                 = (il2cpp_domain_get_t)                 dlsym(h, OBFUSCATE("il2cpp_domain_get"));
    il2cpp_domain_get_assemblies      = (il2cpp_domain_get_assemblies_t)      dlsym(h, OBFUSCATE("il2cpp_domain_get_assemblies"));
    il2cpp_assembly_get_image         = (il2cpp_assembly_get_image_t)         dlsym(h, OBFUSCATE("il2cpp_assembly_get_image"));
    il2cpp_class_from_name            = (il2cpp_class_from_name_t)            dlsym(h, OBFUSCATE("il2cpp_class_from_name"));
    il2cpp_class_get_method_from_name = (il2cpp_class_get_method_from_name_t) dlsym(h, OBFUSCATE("il2cpp_class_get_method_from_name"));
    il2cpp_runtime_invoke             = (il2cpp_runtime_invoke_t)             dlsym(h, OBFUSCATE("il2cpp_runtime_invoke"));
    il2cpp_thread_attach              = (il2cpp_thread_attach_t)              dlsym(h, OBFUSCATE("il2cpp_thread_attach"));
    il2cpp_string_chars               = (il2cpp_string_chars_t)               dlsym(h, OBFUSCATE("il2cpp_string_chars"));
    il2cpp_array_length               = (il2cpp_array_length_t)               dlsym(h, OBFUSCATE("il2cpp_array_length"));
    il2cpp_array_new                  = (il2cpp_array_new_t)                  dlsym(h, OBFUSCATE("il2cpp_array_new"));
    il2cpp_class_get_type             = (il2cpp_class_get_type_t)             dlsym(h, OBFUSCATE("il2cpp_class_get_type"));
    il2cpp_type_get_object            = (il2cpp_type_get_object_t)            dlsym(h, OBFUSCATE("il2cpp_type_get_object"));
    il2cpp_value_box                  = (il2cpp_value_box_t)                  dlsym(h, OBFUSCATE("il2cpp_value_box"));

    if (!il2cpp_domain_get || !il2cpp_runtime_invoke || !il2cpp_domain_get_assemblies) {
        LOGI(OBFUSCATE("core il2cpp symbols missing"));
        return;
    }

    // Ждём ненулевой domain — инициализируется позже, чем грузится libil2cpp.so
    void* domain = nullptr;
    while (!domain) {
        domain = il2cpp_domain_get();
        if (!domain) {
            LOGI(OBFUSCATE("waiting for il2cpp domain..."));
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }
    if (il2cpp_thread_attach) {
        il2cpp_thread_attach(domain);
    }
    LOGI(OBFUSCATE("il2cpp domain ready"));

    size_t count = 0;
    void** assemblies = il2cpp_domain_get_assemblies(domain, &count);

    for (size_t i = 0; i < count; ++i) {
        void* image = il2cpp_assembly_get_image(assemblies[i]);
        if (!texKlass)     texKlass     = il2cpp_class_from_name(image, OBFUSCATE("UnityEngine"), OBFUSCATE("Texture2D"));
        if (!resKlass)     resKlass     = il2cpp_class_from_name(image, OBFUSCATE("UnityEngine"), OBFUSCATE("Resources"));
        if (!byteKlass)    byteKlass    = il2cpp_class_from_name(image, OBFUSCATE("System"), OBFUSCATE("Byte"));
        if (!boolKlass)    boolKlass    = il2cpp_class_from_name(image, OBFUSCATE("System"), OBFUSCATE("Boolean"));
        if (!objKlass)     objKlass     = il2cpp_class_from_name(image, OBFUSCATE("UnityEngine"), OBFUSCATE("Object"));
        if (!imgConvKlass) imgConvKlass = il2cpp_class_from_name(image, OBFUSCATE("UnityEngine"), OBFUSCATE("ImageConversion"));
    }

    if (resKlass) mi_findAll    = il2cpp_class_get_method_from_name(resKlass, OBFUSCATE("FindObjectsOfTypeAll"), 1);
    if (objKlass) mi_objGetName = il2cpp_class_get_method_from_name(objKlass, OBFUSCATE("get_name"), 0);
    if (texKlass) {
        mi_apply      = il2cpp_class_get_method_from_name(texKlass, OBFUSCATE("Apply"), 2);
        mi_isReadable = il2cpp_class_get_method_from_name(texKlass, OBFUSCATE("get_isReadable"), 0);
        if (il2cpp_class_get_type && il2cpp_type_get_object) {
            texType = il2cpp_type_get_object(il2cpp_class_get_type(texKlass));
        }
    }
    // Unity 5.6+: EncodeToPNG/LoadImage — статические методы в ImageConversion
    if (imgConvKlass) {
        mi_encodePNG = il2cpp_class_get_method_from_name(imgConvKlass, OBFUSCATE("EncodeToPNG"), 1);
        mi_loadImage = il2cpp_class_get_method_from_name(imgConvKlass, OBFUSCATE("LoadImage"), 2);
        g_texConvStatic = (mi_encodePNG != nullptr);
    }
    // Фолбэк для старых Unity: инстансные методы в Texture2D
    if (texKlass && !g_texConvStatic) {
        if (!mi_encodePNG) mi_encodePNG = il2cpp_class_get_method_from_name(texKlass, OBFUSCATE("EncodeToPNG"), 0);
        if (!mi_loadImage) mi_loadImage = il2cpp_class_get_method_from_name(texKlass, OBFUSCATE("LoadImage"), 1);
    }

    LOGI(OBFUSCATE("il2cpp resolved: tex=%p imgConv=%p type=%p findAll=%p enc=%p load=%p apply=%p static=%d"),
         texKlass, imgConvKlass, texType, mi_findAll, mi_encodePNG, mi_loadImage, mi_apply, (int)g_texConvStatic);
}

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

// Возвращает число сохранённых PNG (-1 — методы не готовы)
static int DumpTextures() {
    if (!mi_findAll || !texType || !mi_encodePNG || !mi_objGetName) {
        LOGI(OBFUSCATE("dump skipped: missing methods"));
        return -1;
    }

    void* findAllParams[1] = { texType };
    void* exc = nullptr;
    void* arr = il2cpp_runtime_invoke(mi_findAll, nullptr, findAllParams, &exc);
    if (exc || !arr) { LOGI(OBFUSCATE("FindObjectsOfTypeAll failed")); return -1; }

    uint32_t count = il2cpp_array_length(arr);
    LOGI(OBFUSCATE("objects found: %u"), count);

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
            // Для значимых типов invoke возвращает Il2CppObject*, значение по 0x10
            if (!exc && r && *(bool*)((char*)r + kBoxedValueOffset) == false) {
                unreadable++;
                continue;
            }
        }

        // EncodeToPNG: статический -> (tex) при obj=nullptr; инстансный -> obj=tex
        exc = nullptr;
        void* png = nullptr;
        if (g_texConvStatic) {
            void* encParams[1] = { tex };
            png = il2cpp_runtime_invoke(mi_encodePNG, nullptr, encParams, &exc);
        } else {
            png = il2cpp_runtime_invoke(mi_encodePNG, tex, nullptr, &exc);
        }
        if (exc || !png) { skipped++; continue; }

        uint32_t pngSize = il2cpp_array_length(png);
        if (pngSize == 0) { skipped++; continue; }

        std::string fname = SanitizeName(cname) + ".png";
        std::string path = g_dumpDir + "/" + fname;

        FILE* f = fopen(path.c_str(), "wb");
        if (!f) { skipped++; continue; }
        size_t w = fwrite(ArrayData(png), 1, pngSize, f);
        fclose(f);
        if (w == pngSize) saved++;
        else skipped++;
    }

    LOGI(OBFUSCATE("dump done: saved=%d skipped=%d unreadable=%d"), saved, skipped, unreadable);
    return saved;
}

static bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
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

static void ApplyCustomTextures() {
    if (!mi_findAll || !texType || !mi_loadImage || !mi_apply || !byteKlass || !boolKlass || !mi_objGetName || !il2cpp_value_box) return;

    void* findAllParams[1] = { texType };
    void* exc = nullptr;
    void* arr = il2cpp_runtime_invoke(mi_findAll, nullptr, findAllParams, &exc);
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
            // Сравниваем с санитизированным именем — так же, как сохраняли в DumpTextures
            if (cname && want == SanitizeName(cname)) { target = tex; break; }
        }
        if (!target) continue;

        std::vector<uint8_t> png;
        if (!ReadFile(g_modDir + "/" + fname, png)) continue;

        void* byteArr = il2cpp_array_new(byteKlass, (uintptr_t)png.size());
        if (!byteArr) continue;
        memcpy(ArrayData(byteArr), png.data(), png.size());

        // LoadImage: статический -> (tex, byte[]) при obj=nullptr; инстансный -> obj=target
        exc = nullptr;
        if (g_texConvStatic) {
            void* liParams[2] = { target, byteArr };
            il2cpp_runtime_invoke(mi_loadImage, nullptr, liParams, &exc);
        } else {
            void* liParams[1] = { byteArr };
            il2cpp_runtime_invoke(mi_loadImage, target, liParams, &exc);
        }
        if (exc) continue;

        // bool-аргументы для Apply нужно боксить
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

    if (applied > 0) LOGI(OBFUSCATE("apply done: %d"), applied);
}

static void hack_thread() {
    // Путь к логу задаём сразу — записи до выдачи доступа попадут в logcat,
    // а файл начнёт наполняться, как только хранилище станет доступно
    g_logPath = std::string(kStorageRoot) + "/texdump/texmod_log.txt";

    // libil2cpp.so загружается не сразу — ждём (как в рабочем моде)
    while (!isLibraryLoaded(targetLibName)) {
        sleep(1);
    }

    ResolveIl2Cpp();

    if (!texKlass || !il2cpp_runtime_invoke) {
        LOGI(OBFUSCATE("il2cpp not available, thread exit"));
        return;
    }

    g_dumpDir = std::string(kStorageRoot) + "/texdump";
    g_modDir  = std::string(kStorageRoot) + "/texmod";

    // Ждём выдачи MANAGE_EXTERNAL_STORAGE — тумблер могут включить после старта игры
    while (!StorageAccessOK()) {
        LOGI(OBFUSCATE("storage access DENIED - grant All files access in Settings (retry in 10s)"));
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
    LOGI(OBFUSCATE("storage access OK"));

    mkdir(g_dumpDir.c_str(), 0777);
    mkdir(g_modDir.c_str(), 0777);

    if (!DumpExists()) {
        // Дамп не спешим: сцены ещё не прогружены, текстур в памяти может не быть.
        // Повторяем до 12 раз с паузой 30 секунд, пока не появится хоть одна текстура.
        for (int attempt = 1; attempt <= 12; ++attempt) {
            LOGI(OBFUSCATE("dump attempt %d/12"), attempt);
            int savedNow = DumpTextures();
            if (DumpExists()) break;
            if (attempt < 12) std::this_thread::sleep_for(std::chrono::seconds(30));
            (void)savedNow;
        }
    } else {
        LOGI(OBFUSCATE("dump exists, skip"));
    }

    while (true) {
        ApplyCustomTextures();
        std::this_thread::sleep_for(std::chrono::seconds(60));
    }
}

// ============================================================================
// Контракт LGL: определения совпадают с Menu.hpp один-в-один.
// Setup.cpp регистрирует их через RegisterNatives:
//   GetFeatureList -> com/android/support/Menu,       ()[Ljava/lang/String;
//   Changes        -> com/android/support/Preferences (Landroid/content/Context;ILjava/lang/String;IJZLjava/lang/String;)V
// ============================================================================

jobjectArray GetFeatureList(JNIEnv *env, jobject context) {
    (void)context; // TexMod автоматический — тумблеров в меню нет
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
// Точка входа — конструктор библиотеки: запускается автоматически при загрузке
// .so в процесс игры (как в рабочем моде). JNI_OnLoad здесь НЕ определяем —
// он в Setup.cpp.
// ============================================================================

__attribute__((constructor))
static void lib_main() {
    std::thread(hack_thread).detach();
}
