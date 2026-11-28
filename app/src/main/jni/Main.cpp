// Main.cpp — TexMod: inject-only build
// Fix #29: убран наш JNI_OnLoad (уже определён в Setup.cpp) — JavaVM берём
// через официальный API JNI_GetCreatedJavaVMs, конфликтов линковки нет.

#include "Menu/Menu.hpp"
#include "Includes/obfuscate.h"

#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/stat.h>
#include <dirent.h>
#include <string>
#include <vector>

// ==================== логирование ====================
static void LogMsg(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void LogMsg(const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char stamp[16];
    strftime(stamp, sizeof(stamp), "%H:%M:%S", &tmv);
    __android_log_print(ANDROID_LOG_INFO, "TexMod", "%s %s", stamp, line);
    FILE* f = fopen("/storage/emulated/0/texmod_log.txt", "a");
    if (f) { fprintf(f, "%s %s\n", stamp, line); fclose(f); }
}

static void ClearPending(JNIEnv* env) {
    if (env && env->ExceptionCheck()) env->ExceptionClear();
}

// ==================== доступ к JavaVM (без JNI_OnLoad!) ====================
// Setup.cpp шаблона сам определяет JNI_OnLoad и регистрирует нативные методы,
// поэтому здесь JVM получаем через JNI_GetCreatedJavaVMs.
static JNIEnv* GetEnv() {
    typedef jint (*fn_GetVMs)(JavaVM**, jsize, jsize*);
    static fn_GetVMs getVMs = NULL;
    if (!getVMs) getVMs = reinterpret_cast<fn_GetVMs>(dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs"));
    if (!getVMs) return NULL;
    JavaVM* vms[4];
    jsize n = 0;
    if (getVMs(vms, 4, &n) != JNI_OK || n < 1 || !vms[0]) return NULL;
    JNIEnv* env = NULL;
    if (vms[0]->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK && env) return env;
    if (vms[0]->AttachCurrentThread(&env, NULL) == JNI_OK) return env;
    return NULL;
}

// ==================== il2cpp API ====================
typedef void* (*fn_DomainGet)();
typedef void** (*fn_DomainGetAssemblies)(void*, unsigned long long*);
typedef void* (*fn_ThreadAttach)(void*);
typedef void* (*fn_AssemblyGetImage)(void*);
typedef const char* (*fn_ImageGetName)(void*);
typedef int (*fn_ImageGetClassCount)(void*);
typedef void* (*fn_ImageGetClass)(void*, int);
typedef const char* (*fn_ClassGetName)(void*);
typedef const char* (*fn_ClassGetNamespace)(void*);
typedef void* (*fn_ClassGetMethod)(void*, const char*, int);
typedef void* (*fn_ClassGetFieldFromName)(void*, const char*);
typedef void (*fn_FieldGetValue)(void*, void*, void*);
typedef void* (*fn_ArrayNew)(void*, unsigned long long, void*);
typedef void* (*fn_StringNew)(const char*);
typedef unsigned short* (*fn_StringChars)(void*, unsigned long long*);
typedef void* (*fn_RuntimeInvoke)(void*, void*, void**, void*);
typedef void* (*fn_ResolveIcall)(const char*);
typedef bool (*fn_LoadImageIcall)(void*, void*, bool);

static void* g_handle = NULL;
static void* g_domain = NULL;
static fn_DomainGet p_DomainGet = NULL;
static fn_DomainGetAssemblies p_DomainGetAssemblies = NULL;
static fn_ThreadAttach p_ThreadAttach = NULL;
static fn_AssemblyGetImage p_AssemblyGetImage = NULL;
static fn_ImageGetName p_ImageGetName = NULL;
static fn_ImageGetClassCount p_ImageGetClassCount = NULL;
static fn_ImageGetClass p_ImageGetClass = NULL;
static fn_ClassGetName p_ClassGetName = NULL;
static fn_ClassGetNamespace p_ClassGetNamespace = NULL;
static fn_ClassGetMethod p_ClassGetMethod = NULL;
static fn_ClassGetFieldFromName p_ClassGetFieldFromName = NULL;
static fn_FieldGetValue p_FieldGetValue = NULL;
static fn_ArrayNew p_ArrayNew = NULL;
static fn_StringNew p_StringNew = NULL;
static fn_StringChars p_StringChars = NULL;
static fn_RuntimeInvoke p_RuntimeInvoke = NULL;
static fn_ResolveIcall p_ResolveIcall = NULL;
static fn_LoadImageIcall g_loadIcall = NULL;

static void* g_texClass = NULL;
static void* g_objClass = NULL;
static void* g_typeClass = NULL;
static void* g_resClass = NULL;
static void* g_color32Class = NULL;
static void* g_byteClass = NULL;
static void* g_imgConvClass = NULL;

static void* g_findAll = NULL;    // Object.FindObjectsOfTypeAll(Type)
static void* g_typeMeth = NULL;   // Type.GetTypeFromName(string)
static void* g_getName = NULL;    // Object.GetName()
static void* g_apply0 = NULL;     // Texture2D.Apply()
static void* g_load1 = NULL;      // ImageConversion.LoadImage(byte[])
static void* g_rawLoad = NULL;    // Texture2D.LoadRawTextureData(byte[])
static void* g_setPix32 = NULL;   // Texture2D.SetPixels32(Color32[])
static void* g_nameField = NULL;
static void* g_widthField = NULL;
static void* g_heightField = NULL;
static void* g_formatField = NULL;

static volatile bool g_injectEnabled = true;
static long g_applied = 0;
static long g_passNo = 0;

#define GETSYM(var, name) if (!(var)) (var) = reinterpret_cast<decltype(var)>(dlsym(g_handle, name));

// ==================== утилиты ====================
static std::string Utf16ToUtf8(const unsigned short* s, unsigned long long len) {
    std::string out;
    out.reserve((size_t)len);
    for (unsigned long long i = 0; i < len; i++) {
        unsigned short c = s[i];
        if (c < 0x80) out.push_back((char)c);
        else if (c < 0x800) {
            out.push_back((char)(0xC0 | (c >> 6)));
            out.push_back((char)(0x80 | (c & 0x3F)));
        } else {
            out.push_back((char)(0xE0 | (c >> 12)));
            out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

static std::string Il2StrToStd(void* str) {
    if (!str || !p_StringChars) return std::string();
    unsigned long long len = 0;
    unsigned short* chars = p_StringChars(str, &len);
    if (!chars) return std::string();
    return Utf16ToUtf8(chars, len);
}

static unsigned long long ArrayLen(void* arr) {
    if (!arr) return 0;
    size_t sz = 0;
    memcpy(&sz, (char*)arr + 3 * sizeof(void*), sizeof(sz));
    return (unsigned long long)sz;
}

static void* ArrayElem(void* arr, unsigned long long i) {
    if (!arr) return NULL;
    return *(void**)((char*)arr + 4 * sizeof(void*) + (size_t)i * sizeof(void*));
}

static std::string SanitizeName(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        char c = in[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            out.push_back(c);
        else
            out.push_back('_');
    }
    return out;
}

static bool ReadFileBytes(const std::string& path, std::vector<unsigned char>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long sz = ftell(f);
    if (sz <= 0 || sz > 64L * 1024L * 1024L) { fclose(f); return false; }
    fseek(f, 0, SEEK_SET);
    out.resize((size_t)sz);
    size_t rd = fread(out.data(), 1, (size_t)sz, f);
    fclose(f);
    return rd == (size_t)sz;
}

// имя текстуры: сначала поле m_Name (без входа в managed-код — безопасно),
// только если поля нет — invoke GetName с проверкой исключения
static std::string GetTexName(void* tex) {
    if (g_nameField && p_FieldGetValue) {
        void* str = NULL;
        p_FieldGetValue(tex, g_nameField, &str);
        std::string s = Il2StrToStd(str);
        if (!s.empty()) return s;
    }
    if (g_getName && p_RuntimeInvoke) {
        void* exc = NULL;
        void* ret = p_RuntimeInvoke(g_getName, tex, NULL, &exc);
        if (!exc && ret) return Il2StrToStd(ret);
        ClearPending(GetEnv());
    }
    return std::string();
}

// ==================== декод PNG через Android ====================
static bool JniDecodePngToRgba(JNIEnv* env, const unsigned char* data, size_t len,
                               int tw, int th, std::vector<unsigned char>& out) {
    if (!env || !data || len == 0 || tw <= 0 || th <= 0) return false;
    bool ok = false;
    jclass bmpCls = env->FindClass("android/graphics/Bitmap");
    jclass facCls = env->FindClass("android/graphics/BitmapFactory");
    jmethodID decode = NULL, gw = NULL, gh = NULL, getpix = NULL, recycle = NULL, scale = NULL;
    jbyteArray jarr = NULL;
    jintArray jpix = NULL;
    jobject bmp = NULL;
    jint* raw = NULL;
    do {
        if (!bmpCls || !facCls) break;
        decode = env->GetStaticMethodID(facCls, "decodeByteArray", "([BII)Landroid/graphics/Bitmap;");
        gw = env->GetMethodID(bmpCls, "getWidth", "()I");
        gh = env->GetMethodID(bmpCls, "getHeight", "()I");
        getpix = env->GetMethodID(bmpCls, "getPixels", "([IIIIIII)V");
        recycle = env->GetMethodID(bmpCls, "recycle", "()V");
        scale = env->GetStaticMethodID(bmpCls, "createScaledBitmap",
                                       "(Landroid/graphics/Bitmap;IIZ)Landroid/graphics/Bitmap;");
        if (!decode || !gw || !gh || !getpix || !recycle) break;
        jarr = env->NewByteArray((jsize)len);
        if (!jarr) break;
        env->SetByteArrayRegion(jarr, 0, (jsize)len, (const jbyte*)data);
        bmp = env->CallStaticObjectMethod(facCls, decode, jarr, (jint)0, (jint)len);
        if (env->ExceptionCheck()) { env->ExceptionClear(); bmp = NULL; }
        if (!bmp) break;
        int bw = env->CallIntMethod(bmp, gw);
        int bh = env->CallIntMethod(bmp, gh);
        if (bw > 0 && bh > 0 && (bw != tw || bh != th) && scale) {
            jobject scaled = env->CallStaticObjectMethod(bmpCls, scale, bmp, (jint)tw, (jint)th, (jboolean)JNI_TRUE);
            if (env->ExceptionCheck()) { env->ExceptionClear(); scaled = NULL; }
            if (scaled) {
                if (recycle) env->CallVoidMethod(bmp, recycle);
                env->DeleteLocalRef(bmp);
                bmp = scaled;
            }
        }
        jpix = env->NewIntArray(tw * th);
        if (!jpix) break;
        env->CallVoidMethod(bmp, getpix, jpix, (jint)0, (jint)tw, (jint)0, (jint)0, (jint)tw, (jint)th);
        if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
        raw = env->GetIntArrayElements(jpix, NULL);
        if (!raw) break;
        out.resize((size_t)tw * (size_t)th * 4);
        for (int y = 0; y < th; y++) {
            for (int x = 0; x < tw; x++) {
                jint p = raw[y * tw + x];
                unsigned char* d = &out[((size_t)y * (size_t)tw + (size_t)x) * 4];
                d[0] = (unsigned char)((p >> 16) & 0xFF); // R
                d[1] = (unsigned char)((p >> 8) & 0xFF);  // G
                d[2] = (unsigned char)(p & 0xFF);         // B
                d[3] = (unsigned char)((p >> 24) & 0xFF); // A
            }
        }
        ok = true;
    } while (0);
    if (raw && jpix) env->ReleaseIntArrayElements(jpix, raw, JNI_ABORT);
    if (bmp && recycle) env->CallVoidMethod(bmp, recycle);
    if (jarr) env->DeleteLocalRef(jarr);
    if (jpix) env->DeleteLocalRef(jpix);
    if (bmp) env->DeleteLocalRef(bmp);
    ClearPending(env);
    return ok;
}

// ==================== заливка пикселей ====================
static bool PushPixels(void* tex, const std::vector<unsigned char>& rgba, int w, int h) {
    if (!tex || rgba.empty() || !p_ArrayNew || !p_RuntimeInvoke) return false;
    if (rgba.size() != (size_t)w * (size_t)h * 4) return false;
    void* exc = NULL;
    // путь 2: LoadRawTextureData
    if (g_rawLoad && g_byteClass) {
        void* arr = p_ArrayNew(g_byteClass, (unsigned long long)rgba.size(), NULL);
        if (arr) {
            memcpy((char*)arr + 4 * sizeof(void*), rgba.data(), rgba.size());
            void* args[1] = { &arr };
            exc = NULL;
            p_RuntimeInvoke(g_rawLoad, tex, args, &exc);
            if (!exc && g_apply0) { exc = NULL; p_RuntimeInvoke(g_apply0, tex, NULL, &exc); }
            if (!exc) return true;
            LogMsg("raw push threw");
        }
    }
    // путь 3: SetPixels32
    if (g_setPix32 && g_color32Class) {
        void* arr = p_ArrayNew(g_color32Class, (unsigned long long)w * (unsigned long long)h, NULL);
        if (arr) {
            memcpy((char*)arr + 4 * sizeof(void*), rgba.data(), rgba.size());
            void* args[1] = { &arr };
            exc = NULL;
            p_RuntimeInvoke(g_setPix32, tex, args, &exc);
            if (!exc && g_apply0) { exc = NULL; p_RuntimeInvoke(g_apply0, tex, NULL, &exc); }
            if (!exc) return true;
            LogMsg("setpix threw");
        }
    }
    return false;
}

// путь 1: LoadImage (если класс уцелел от стриппинга)
static bool PushPng(void* tex, const std::vector<unsigned char>& png) {
    if (!tex || png.empty()) return false;
    if (g_loadIcall) {
        void* arr = (g_byteClass && p_ArrayNew) ? p_ArrayNew(g_byteClass, (unsigned long long)png.size(), NULL) : NULL;
        if (!arr) return false;
        memcpy((char*)arr + 4 * sizeof(void*), png.data(), png.size());
        bool r = g_loadIcall(tex, arr, true);
        void* exc = NULL;
        if (r && g_apply0) { exc = NULL; p_RuntimeInvoke(g_apply0, tex, NULL, &exc); }
        return r && !exc;
    }
    if (g_load1 && p_RuntimeInvoke) {
        void* arr = (g_byteClass && p_ArrayNew) ? p_ArrayNew(g_byteClass, (unsigned long long)png.size(), NULL) : NULL;
        if (!arr) return false;
        memcpy((char*)arr + 4 * sizeof(void*), png.data(), png.size());
        void* args[1] = { &arr };
        void* exc = NULL;
        p_RuntimeInvoke(g_load1, NULL, args, &exc);
        if (!exc && g_apply0) { exc = NULL; p_RuntimeInvoke(g_apply0, tex, NULL, &exc); }
        return !exc;
    }
    return false;
}

// ==================== защитный барьер от SIGSEGV ====================
static __thread sigjmp_buf g_segvJmp;
static __thread volatile bool g_segvActive = false;
static void SegvHandler(int) {
    if (g_segvActive) siglongjmp(g_segvJmp, 1);
    _exit(0);
}

static void InjectPassBody() {
    if (!p_ThreadAttach || !g_domain) return;
    p_ThreadAttach(g_domain);
    if (!g_typeMeth || !g_findAll || !g_texClass || !p_StringNew) return;

    void* sName = p_StringNew("UnityEngine.Texture2D");
    if (!sName) return;
    void* argsT[1] = { &sName };
    void* exc = NULL;
    void* typeObj = p_RuntimeInvoke(g_typeMeth, NULL, argsT, &exc);
    if (exc || !typeObj) { LogMsg("type resolve failed"); ClearPending(GetEnv()); return; }

    void* argsF[1] = { &typeObj };
    exc = NULL;
    void* arr = p_RuntimeInvoke(g_findAll, NULL, argsF, &exc);
    if (exc || !arr) { ClearPending(GetEnv()); return; }

    unsigned long long cnt = ArrayLen(arr);
    JNIEnv* jenv = GetEnv();

    for (unsigned long long i = 0; i < cnt; i++) {
        void* tex = ArrayElem(arr, i);
        if (!tex) continue;
        std::string name = GetTexName(tex);
        if (name.empty()) continue;

        int w = 0, h = 0;
        if (g_widthField) p_FieldGetValue(tex, g_widthField, &w);
        if (g_heightField) p_FieldGetValue(tex, g_heightField, &h);
        if (w <= 0 || h <= 0 || w > 8192 || h > 8192) continue;

        std::string base = SanitizeName(name);
        std::vector<unsigned char> bytes;
        bool isRaw = false;
        if (ReadFileBytes("/storage/emulated/0/texmod/" + base + ".png", bytes) ||
            ReadFileBytes("/storage/emulated/0/texmod/" + name + ".png", bytes)) {
            isRaw = false;
        } else if (ReadFileBytes("/storage/emulated/0/texmod/" + base + ".rgba", bytes)) {
            isRaw = true;
        } else {
            continue;
        }

        bool injected = false;
        if (!isRaw) {
            if (PushPng(tex, bytes)) {
                injected = true;
            } else if (jenv) {
                std::vector<unsigned char> px;
                if (JniDecodePngToRgba(jenv, bytes.data(), bytes.size(), w, h, px) &&
                    PushPixels(tex, px, w, h))
                    injected = true;
            }
        } else {
            if (bytes.size() == (size_t)w * (size_t)h * 4 && PushPixels(tex, bytes, w, h))
                injected = true;
        }

        if (injected) {
            g_applied++;
            LogMsg("injected: %s (%dx%d)", name.c_str(), w, h);
        }
    }
    LogMsg("heartbeat: pass=%ld applied=%ld textures=%llu", g_passNo, g_applied, cnt);
}

static void InjectPass() {
    struct sigaction sa, old;
    memset(&sa, 0, sizeof(sa));
    memset(&old, 0, sizeof(old));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_ONSTACK | SA_RESTART;
    sa.sa_handler = SegvHandler;
    sigaction(SIGSEGV, &sa, &old);
    if (sigsetjmp(g_segvJmp, 1) != 0) {
        g_segvActive = false;
        sigaction(SIGSEGV, &old, NULL);
        LogMsg("segv caught in inject pass, skipped");
        return;
    }
    g_segvActive = true;
    do { InjectPassBody(); } while (0);
    g_segvActive = false;
    sigaction(SIGSEGV, &old, NULL);
}

// ==================== резолв il2cpp ====================
static bool ResolveIl2Cpp() {
    if (g_domain) return true;
    if (!g_handle) {
        g_handle = dlopen("libil2cpp.so", RTLD_NOW);
        if (!g_handle) return false;
        LogMsg("libil2cpp.so loaded");
    }
    GETSYM(p_DomainGet, "il2cpp_domain_get");
    GETSYM(p_DomainGetAssemblies, "il2cpp_domain_get_assemblies");
    GETSYM(p_ThreadAttach, "il2cpp_thread_attach");
    GETSYM(p_AssemblyGetImage, "il2cpp_assembly_get_image");
    GETSYM(p_ImageGetName, "il2cpp_image_get_name");
    GETSYM(p_ImageGetClassCount, "il2cpp_image_get_class_count");
    GETSYM(p_ImageGetClass, "il2cpp_image_get_class");
    GETSYM(p_ClassGetName, "il2cpp_class_get_name");
    GETSYM(p_ClassGetNamespace, "il2cpp_class_get_namespace");
    GETSYM(p_ClassGetMethod, "il2cpp_class_get_method_from_name");
    GETSYM(p_ClassGetFieldFromName, "il2cpp_class_get_field_from_name");
    GETSYM(p_FieldGetValue, "il2cpp_field_get_value");
    GETSYM(p_ArrayNew, "il2cpp_array_new");
    GETSYM(p_StringNew, "il2cpp_string_new");
    GETSYM(p_StringChars, "il2cpp_string_chars");
    GETSYM(p_RuntimeInvoke, "il2cpp_runtime_invoke");
    GETSYM(p_ResolveIcall, "il2cpp_resolve_icall");
    if (!p_DomainGet) return false;
    if (!g_domain) g_domain = p_DomainGet();
    if (!g_domain) return false;
    LogMsg("il2cpp domain ready");

    // ждём стабилизации списка сборок (два одинаковых счётчика подряд)
    unsigned long long nn = 0, prev = 0;
    void** asms = NULL;
    for (int tries = 0; tries < 120; tries++) {
        asms = p_DomainGetAssemblies ? p_DomainGetAssemblies(g_domain, &nn) : NULL;
        if (asms && nn > 0 && nn == prev) break;
        prev = nn;
        usleep(100000);
    }
    return g_domain != NULL;
}

static void ScanAndResolve() {
    if (!p_DomainGetAssemblies || !p_ImageGetClassCount || !p_ImageGetClass) return;
    void** assemblies = NULL;
    unsigned long long n = 0;
    assemblies = p_DomainGetAssemblies(g_domain, &n);
    if (!assemblies || n == 0) return;

    unsigned long long total = 0;
    for (unsigned long long i = 0; i < n; i++) {
        void* image = p_AssemblyGetImage(assemblies[i]);
        if (!image) continue;
        const char* imgName = p_ImageGetName(image);
        LogMsg("asm[%llu]: %s", i, imgName ? imgName : "?");
        int cnt = p_ImageGetClassCount(image);
        for (int j = 0; j < cnt; j++) {
            void* cls = p_ImageGetClass(image, j);
            if (!cls) continue;
            total++;
            const char* ns = p_ClassGetNamespace(cls);
            const char* nm = p_ClassGetName(cls);
            if (!nm) continue;
            bool isU = ns && strcmp(ns, "UnityEngine") == 0;
            bool isS = ns && strcmp(ns, "System") == 0;
            if (isU) {
                if (!g_texClass && !strcmp(nm, "Texture2D")) g_texClass = cls;
                else if (!g_objClass && !strcmp(nm, "Object")) g_objClass = cls;
                else if (!g_typeClass && !strcmp(nm, "Type")) g_typeClass = cls;
                else if (!g_resClass && !strcmp(nm, "Resources")) g_resClass = cls;
                else if (!g_color32Class && !strcmp(nm, "Color32")) g_color32Class = cls;
                else if (!g_imgConvClass && !strcmp(nm, "ImageConversion")) g_imgConvClass = cls;
            } else if (isS) {
                if (!g_byteClass && !strcmp(nm, "Byte")) g_byteClass = cls;
            }
        }
    }
    LogMsg("classes scanned: %llu", total);

    if (g_objClass) {
        g_getName = p_ClassGetMethod(g_objClass, "GetName", 0);
        g_findAll = p_ClassGetMethod(g_objClass, "FindObjectsOfTypeAll", 1);
        g_nameField = p_ClassGetFieldFromName(g_objClass, "m_Name");
    }
    if (g_typeClass) g_typeMeth = p_ClassGetMethod(g_typeClass, "GetTypeFromName", 1);
    if (g_resClass) p_ClassGetMethod(g_resClass, "Load", 1);
    if (g_texClass) {
        g_apply0 = p_ClassGetMethod(g_texClass, "Apply", 0);
        g_rawLoad = p_ClassGetMethod(g_texClass, "LoadRawTextureData", 1);
        g_setPix32 = p_ClassGetMethod(g_texClass, "SetPixels32", 1);
        g_widthField = p_ClassGetFieldFromName(g_texClass, "m_Width");
        g_heightField = p_ClassGetFieldFromName(g_texClass, "m_Height");
        g_formatField = p_ClassGetFieldFromName(g_texClass, "m_Format");
    }
    if (g_imgConvClass) g_load1 = p_ClassGetMethod(g_imgConvClass, "LoadImage", 1);
    if (p_ResolveIcall)
        g_loadIcall = reinterpret_cast<fn_LoadImageIcall>(
            p_ResolveIcall("UnityEngine.ImageConversion::LoadImage(System.Byte[],System.Boolean)"));

    LogMsg("resolved: tex=%p res=%p imgConv=%p findAll=%p getName=%p apply0=%p load3=%p load1=%p icall=%p",
           (void*)g_texClass, (void*)g_resClass, (void*)g_imgConvClass, (void*)g_findAll,
           (void*)g_getName, (void*)g_apply0, (void*)g_setPix32, (void*)g_load1, (void*)g_loadIcall);
    LogMsg("fallback: color32=%p set32=%p raw=%p byte=%p w=%p h=%p",
           (void*)g_color32Class, (void*)g_setPix32, (void*)g_rawLoad,
           (void*)g_byteClass, (void*)g_widthField, (void*)g_heightField);
}

// ==================== рабочий поток ====================
static void* TexThread(void*) {
    for (int i = 0; i < 240; i++) {
        if (ResolveIl2Cpp()) break;
        usleep(500000);
    }
    if (!g_domain) { LogMsg("il2cpp not found, giving up"); return NULL; }
    ScanAndResolve();
    while (true) {
        if (g_injectEnabled) {
            g_passNo++;
            InjectPass();
        } else {
            LogMsg("heartbeat: pass=%ld applied=%ld (disabled)", g_passNo, g_applied);
        }
        sleep(10);
    }
    return NULL;
}

__attribute__((constructor)) static void TexModInit() {
    LogMsg("mod start (inject-only build)");
    if (access("/storage/emulated/0/texmod", F_OK) == 0)
        LogMsg("storage access OK");
    else
        LogMsg("texmod dir missing, create /storage/emulated/0/texmod");
    pthread_t t;
    if (pthread_create(&t, NULL, TexThread, NULL) == 0) pthread_detach(t);
}

// ==================== меню (сигнатуры строго из Menu.hpp) ====================
jobjectArray GetFeatureList(JNIEnv* env, jobject context) {
    ClearPending(env);
    (void)context;
    const char* features[] = {
        OBFUSCATE("Category_Textures"),
        OBFUSCATE("0_Toggle_Inject Textures"),
    };
    int total = (int)(sizeof features / sizeof features[0]);
    jclass strCls = env->FindClass("java/lang/String");
    if (!strCls) { ClearPending(env); return NULL; }
    jobjectArray ret = env->NewObjectArray(total, strCls, env->NewStringUTF(""));
    if (!ret || env->ExceptionCheck()) { ClearPending(env); return NULL; }
    for (int i = 0; i < total; i++)
        env->SetObjectArrayElement(ret, i, env->NewStringUTF(features[i]));
    ClearPending(env);
    return ret;
}

void Changes(JNIEnv* env, jclass clazz, jobject obj,
             jint featNum, jstring featName, jint value, jlong Lvalue,
             jboolean boolean, jstring str) {
    ClearPending(env);
    (void)clazz; (void)obj; (void)value; (void)Lvalue; (void)str;
    if (featName) {
        const char* n = env->GetStringUTFChars(featName, NULL);
        if (n) {
            LogMsg("change: num=%d name=%s", featNum, n);
            env->ReleaseStringUTFChars(featName, n);
        }
    }
    if (featNum == 0) {
        g_injectEnabled = (boolean != JNI_FALSE);
        LogMsg("inject enabled: %d", g_injectEnabled ? 1 : 0);
    }
    ClearPending(env);
}
