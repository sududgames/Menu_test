// Main.cpp — real-time texture injector for Unity IL2CPP game
// Contract (Menu.hpp): jobjectArray GetFeatureList(JNIEnv*, jobject);
//                      void Changes(JNIEnv*, jclass, jobject, jint, jstring, jint, jlong, jboolean, jstring);
// NOTE: no JNI_OnLoad here — it already exists in Menu/Setup.cpp (duplicate-symbol fix).

#include <jni.h>
#include <android/log.h>

#include <dlfcn.h>
#include <pthread.h>
#include <dirent.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/stat.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <set>

#include "Includes/obfuscate.h"
#include "Menu/Menu.hpp"

#define TAG "TexMod"
static const char* kLogPath = "/storage/emulated/0/texmod_log.txt";
static const char* kTexDir  = "/storage/emulated/0/texmod";

// ============================== logging ==============================

static FILE* g_logFile = NULL;
static pthread_mutex_t g_logMx = PTHREAD_MUTEX_INITIALIZER;

static void LogF(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, TAG, "%s", buf);
    pthread_mutex_lock(&g_logMx);
    if (!g_logFile) g_logFile = fopen(kLogPath, "a");
    if (g_logFile) { fprintf(g_logFile, "%s\n", buf); fflush(g_logFile); }
    pthread_mutex_unlock(&g_logMx);
}

// ============================== il2cpp glue ==============================

static void* g_il2cppHandle = NULL;

typedef struct Il2CppDomain_    Il2CppDomain;
typedef struct Il2CppAssembly_  Il2CppAssembly;
typedef struct Il2CppImage_     Il2CppImage;
typedef struct Il2CppObject_    Il2CppObject;

typedef Il2CppDomain*   (*fn_DomainGet)();
typedef Il2CppAssembly**(*fn_DomainGetAssemblies)(Il2CppDomain*, size_t*);
typedef Il2CppImage*    (*fn_AsmGetImage)(Il2CppAssembly*);
typedef const char*     (*fn_ImageGetName)(Il2CppImage*);
typedef size_t          (*fn_ImageGetClassCount)(Il2CppImage*);
typedef void*           (*fn_ImageGetClass)(Il2CppImage*, size_t);
typedef const char*     (*fn_ClassGetName)(void*);
typedef const char*     (*fn_ClassGetNamespace)(void*);
typedef void*           (*fn_ClassGetMethod)(void*, const char*, int);
typedef void*           (*fn_ClassGetField)(void*, const char*);
typedef void*           (*fn_ClassGetType)(void*);
typedef void*           (*fn_TypeGetObject)(void*);
typedef void*           (*fn_ThreadAttach)(Il2CppDomain*);
typedef void*           (*fn_ThreadCurrent)();
typedef void*           (*fn_RuntimeInvoke)(void*, void*, void**, void**);
typedef void            (*fn_FieldGetValue)(void*, void*, void*);
typedef void*           (*fn_ArrayNew)(void*, unsigned long, void*);

static fn_DomainGet           p_DomainGet = NULL;
static fn_DomainGetAssemblies p_DomainGetAssemblies = NULL;
static fn_AsmGetImage         p_AsmGetImage = NULL;
static fn_ImageGetName        p_ImageGetName = NULL;
static fn_ImageGetClassCount  p_ImageGetClassCount = NULL;
static fn_ImageGetClass       p_ImageGetClass = NULL;
static fn_ClassGetName        p_ClassGetName = NULL;
static fn_ClassGetNamespace   p_ClassGetNamespace = NULL;
static fn_ClassGetMethod      p_ClassGetMethod = NULL;
static fn_ClassGetField       p_ClassGetField = NULL;
static fn_ClassGetType        p_ClassGetType = NULL;
static fn_TypeGetObject       p_TypeGetObject = NULL;
static fn_ThreadAttach        p_ThreadAttach = NULL;
static fn_ThreadCurrent       p_ThreadCurrent = NULL;
static fn_RuntimeInvoke       p_RuntimeInvoke = NULL;
static fn_FieldGetValue       p_FieldGetValue = NULL;
static fn_ArrayNew            p_ArrayNew = NULL;

#define GETSYM(var, name) if (!(var)) (var) = reinterpret_cast<decltype(var)>(dlsym(g_il2cppHandle, name));

static bool LoadIl2Cpp() {
    if (g_il2cppHandle) return true;
    g_il2cppHandle = dlopen("libil2cpp.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g_il2cppHandle) {
        FILE* f = fopen("/proc/self/maps", "r");
        if (f) {
            char line[1024];
            while (fgets(line, sizeof(line), f)) {
                if (!strstr(line, "libil2cpp.so")) continue;
                const char* sp = strrchr(line, '/');
                if (!sp) continue;
                char path[1024];
                size_t n = strcspn(sp, " \n");
                if (n >= sizeof(path)) continue;
                memcpy(path, sp, n);
                path[n] = 0;
                g_il2cppHandle = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
                if (!g_il2cppHandle) g_il2cppHandle = dlopen(path, RTLD_NOW);
                if (g_il2cppHandle) break;
            }
            fclose(f);
        }
    }
    if (g_il2cppHandle) LogF("libil2cpp.so loaded");
    return g_il2cppHandle != NULL;
}

// resolved classes/methods/fields
static void* g_texClass = NULL;
static void* g_objClass = NULL;
static void* g_imgConvClass = NULL;
static void* g_byteClass = NULL;
static void* g_color32Class = NULL;

static void* g_getNameM   = NULL; // UnityEngine.Object::GetName()
static void* g_findAllM   = NULL; // UnityEngine.Object::FindObjectsOfTypeAll(Type)
static void* g_applyM     = NULL; // Texture2D::Apply()
static void* g_loadImageM = NULL; // ImageConversion::LoadImage(byte[])   (path 1)
static void* g_loadRawM   = NULL; // Texture2D::LoadRawTextureData(byte[]) (path 2)
static void* g_setPix32M  = NULL; // Texture2D::SetPixels32(Color32[])     (path 3)
static void* g_getWM      = NULL; // Texture2D::get_width()
static void* g_getHM      = NULL; // Texture2D::get_height()
static void* g_nameField  = NULL; // UnityEngine.Object::m_Name (optional)

static void* g_texTypeObj = NULL;

static volatile bool g_injectEnabled = true;
static volatile bool g_running = true;
static Il2CppDomain* g_domain = NULL;

static bool ResolveIl2Cpp() {
    if (!LoadIl2Cpp()) return false;
    GETSYM(p_DomainGet, "il2cpp_domain_get");
    GETSYM(p_DomainGetAssemblies, "il2cpp_domain_get_assemblies");
    GETSYM(p_AsmGetImage, "il2cpp_assembly_get_image");
    GETSYM(p_ImageGetName, "il2cpp_image_get_name");
    GETSYM(p_ImageGetClassCount, "il2cpp_image_get_class_count");
    GETSYM(p_ImageGetClass, "il2cpp_image_get_class");
    GETSYM(p_ClassGetName, "il2cpp_class_get_name");
    GETSYM(p_ClassGetNamespace, "il2cpp_class_get_namespace");
    GETSYM(p_ClassGetMethod, "il2cpp_class_get_method_from_name");
    GETSYM(p_ClassGetField, "il2cpp_class_get_field_from_name");
    GETSYM(p_ClassGetType, "il2cpp_class_get_type");
    GETSYM(p_TypeGetObject, "il2cpp_type_get_object");
    GETSYM(p_ThreadAttach, "il2cpp_thread_attach");
    GETSYM(p_ThreadCurrent, "il2cpp_thread_current");
    GETSYM(p_RuntimeInvoke, "il2cpp_runtime_invoke");
    GETSYM(p_FieldGetValue, "il2cpp_field_get_value");
    GETSYM(p_ArrayNew, "il2cpp_array_new");
    if (!p_DomainGet || !p_DomainGetAssemblies || !p_AsmGetImage ||
        !p_ImageGetClass || !p_ImageGetClassCount || !p_ClassGetMethod) {
        LogF("il2cpp api incomplete");
        return false;
    }

    Il2CppDomain* dom = p_DomainGet();
    if (!dom) return false;

    // wait until the assembly list is stable (two identical counts in a row)
    size_t n = 0, prev = 0;
    Il2CppAssembly** asms = NULL;
    for (int tries = 0; tries < 120; tries++) {
        asms = p_DomainGetAssemblies(dom, &n);
        if (asms && n > 0 && n == prev) break;
        prev = n;
        usleep(100000);
    }
    if (!asms || !n) return false;

    if (g_texClass) return true; // already resolved

    LogF("il2cpp domain ready");
    int scanned = 0;
    for (size_t a = 0; a < n; a++) {
        Il2CppImage* img = asms[a] ? p_AsmGetImage(asms[a]) : NULL;
        if (!img) continue;
        (void)p_ImageGetName; // image names not needed for class resolution
        size_t cc = p_ImageGetClassCount(img);
        for (size_t c = 0; c < cc; c++) {
            void* cls = p_ImageGetClass(img, c);
            if (!cls) continue;
            scanned++;
            const char* ns = p_ClassGetNamespace(cls);
            const char* nm = p_ClassGetName(cls);
            if (!ns || !nm) continue;
            if (!strcmp(nm, "Texture2D") && !strcmp(ns, "UnityEngine")) g_texClass = cls;
            else if (!strcmp(nm, "Object") && !strcmp(ns, "UnityEngine")) g_objClass = cls;
            else if (!strcmp(nm, "ImageConversion") && !strcmp(ns, "UnityEngine")) g_imgConvClass = cls;
            else if (!strcmp(nm, "Byte") && !strcmp(ns, "System")) g_byteClass = cls;
            else if (!strcmp(nm, "Color32") && !strcmp(ns, "UnityEngine")) g_color32Class = cls;
        }
    }
    LogF("classes scanned: %d", scanned);

    if (!g_texClass) { LogF("Texture2D class not found"); return false; }

    if (g_objClass) {
        g_getNameM = p_ClassGetMethod(g_objClass, "GetName", 0);
        g_findAllM = p_ClassGetMethod(g_objClass, "FindObjectsOfTypeAll", 1);
        g_nameField = p_ClassGetField ? p_ClassGetField(g_objClass, "m_Name") : NULL;
    }
    g_applyM     = p_ClassGetMethod(g_texClass, "Apply", 0);
    g_loadRawM   = p_ClassGetMethod(g_texClass, "LoadRawTextureData", 1);
    g_setPix32M  = p_ClassGetMethod(g_texClass, "SetPixels32", 1);
    g_getWM      = p_ClassGetMethod(g_texClass, "get_width", 0);
    g_getHM      = p_ClassGetMethod(g_texClass, "get_height", 0);
    if (g_imgConvClass) g_loadImageM = p_ClassGetMethod(g_imgConvClass, "LoadImage", 1);

    if (p_ClassGetType && p_TypeGetObject) {
        void* t = p_ClassGetType(g_texClass);
        if (t) g_texTypeObj = p_TypeGetObject(t);
    }

    LogF("resolved: tex=%p obj=%p imgConv=%p findAll=%p getName=%p apply=%p load1=%p",
         g_texClass, g_objClass, g_imgConvClass, g_findAllM, g_getNameM, g_applyM, g_loadImageM);
    LogF("fallback: raw=%p set32=%p color32=%p w=%p h=%p nameField=%p",
         g_loadRawM, g_setPix32M, g_color32Class, g_getWM, g_getHM, g_nameField);
    return true;
}

// ============================== JNI helpers ==============================

static JavaVM* g_jvm = NULL;
typedef int (*fn_GetCreatedJavaVMs)(JavaVM**, int, int*);

static JNIEnv* GetJniEnv() {
    if (!g_jvm) {
        fn_GetCreatedJavaVMs f =
            reinterpret_cast<fn_GetCreatedJavaVMs>(dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs"));
        if (f) {
            JavaVM* vms[4] = { NULL, NULL, NULL, NULL };
            int cnt = 0;
            if (f(vms, 4, &cnt) == JNI_OK && cnt > 0) g_jvm = vms[0];
        }
    }
    if (!g_jvm) return NULL;
    JNIEnv* env = NULL;
    if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK && env) return env;
    env = NULL;
    if (g_jvm->AttachCurrentThread((void**)&env, NULL) == JNI_OK && env) return env;
    return NULL;
}

// il2cpp string -> std::string (Il2CppObject {klass,monitor,len(0x10),chars(0x14)})
static std::string ToUtf8(void* strObj) {
    if (!strObj) return "";
    int len = *(int*)((char*)strObj + 0x10);
    if (len <= 0 || len > 4096) return "";
    const char16_t* s = (const char16_t*)((char*)strObj + 0x14);
    std::string out;
    out.reserve((size_t)len);
    for (int i = 0; i < len; i++) {
        char16_t c = s[i];
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
        else { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
    }
    return out;
}

static int ReadIntInvoke(void* obj, void* method) {
    if (!obj || !method || !p_RuntimeInvoke) return 0;
    void* exc = NULL;
    void* r = p_RuntimeInvoke(method, obj, NULL, &exc);
    if (exc || !r) return 0;
    return *(int*)((char*)r + 0x10); // boxed int payload
}

static std::string GetTexName(void* tex) {
    if (!tex) return "";
    // primary: direct field read (no managed call, cannot crash)
    if (g_nameField && p_FieldGetValue) {
        void* strObj = NULL;
        p_FieldGetValue(tex, g_nameField, &strObj);
        std::string s = ToUtf8(strObj);
        if (!s.empty()) return s;
    }
    // fallback: managed invoke, exception-checked
    if (g_getNameM && p_RuntimeInvoke) {
        void* exc = NULL;
        void* r = p_RuntimeInvoke(g_getNameM, tex, NULL, &exc);
        if (!exc && r) return ToUtf8(r);
    }
    return "";
}

static bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 64 * 1024 * 1024) { fclose(f); return false; }
    out.resize((size_t)sz);
    size_t rd = fread(out.data(), 1, out.size(), f);
    fclose(f);
    if (rd != out.size()) out.clear();
    return !out.empty();
}

static int CountTexmodFiles() {
    DIR* d = opendir(kTexDir);
    if (!d) return -1;
    int cnt = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        cnt++;
    }
    closedir(d);
    return cnt;
}

static void* MakeByteArray(const std::vector<uint8_t>& data) {
    if (!g_byteClass || !p_ArrayNew || data.empty()) return NULL;
    void* arr = p_ArrayNew(g_byteClass, (unsigned long)data.size(), NULL);
    if (!arr) return NULL;
    memcpy((char*)arr + 0x20, data.data(), data.size());
    return arr;
}

static bool ApplyTex(void* tex) {
    if (!g_applyM || !p_RuntimeInvoke) return false;
    void* exc = NULL;
    p_RuntimeInvoke(g_applyM, tex, NULL, &exc);
    if (exc) { LogF("apply exc"); return false; }
    return true;
}

// PNG -> RGBA via Android BitmapFactory (works even when Unity stripping removed LoadImage)
static bool DecodePngToRgba(JNIEnv* env, const std::vector<uint8_t>& png, int tw, int th, std::vector<uint8_t>& out) {
    if (!env || png.empty() || tw <= 0 || th <= 0 || tw * th > 8192 * 8192) return false;
    bool ok = false;
    jclass fCls = NULL, bCls = NULL;
    jbyteArray jarr = NULL;
    jintArray jintArr = NULL;
    jint* ibuf = NULL;
    jobject bmp = NULL, scaled = NULL;
    jmethodID decodeM = NULL, scaleM = NULL, getPixM = NULL, recycleM = NULL;
    do {
        fCls = env->FindClass("android/graphics/BitmapFactory");
        if (!fCls || env->ExceptionCheck()) { if (fCls) env->ExceptionClear(); break; }
        decodeM = env->GetStaticMethodID(fCls, "decodeByteArray", "([BII)Landroid/graphics/Bitmap;");
        if (!decodeM) break;
        jarr = env->NewByteArray((jsize)png.size());
        if (!jarr) break;
        env->SetByteArrayRegion(jarr, 0, (jsize)png.size(), (const jbyte*)png.data());
        bmp = env->CallStaticObjectMethod(fCls, decodeM, jarr, 0, (jint)png.size());
        if (!bmp || env->ExceptionCheck()) { env->ExceptionClear(); break; }
        bCls = env->FindClass("android/graphics/Bitmap");
        if (!bCls || env->ExceptionCheck()) { env->ExceptionClear(); break; }
        scaleM = env->GetStaticMethodID(bCls, "createScaledBitmap",
                                        "(Landroid/graphics/Bitmap;IIZ)Landroid/graphics/Bitmap;");
        if (!scaleM) break;
        scaled = env->CallStaticObjectMethod(bCls, scaleM, bmp, (jint)tw, (jint)th, JNI_TRUE);
        if (!scaled || env->ExceptionCheck()) { env->ExceptionClear(); break; }
        getPixM = env->GetMethodID(bCls, "getPixels", "([IIIII)V");
        if (!getPixM) break;
        jintArr = env->NewIntArray(tw * th);
        if (!jintArr) break;
        env->CallObjectMethod(scaled, getPixM, jintArr, 0, (jint)tw, 0, 0, (jint)tw, (jint)th);
        if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
        jsize plen = env->GetArrayLength(jintArr);
        if (plen != tw * th) break;
        ibuf = (jint*)malloc((size_t)plen * sizeof(jint));
        if (!ibuf) break;
        env->GetIntArrayRegion(jintArr, 0, plen, ibuf);
        out.resize((size_t)tw * (size_t)th * 4);
        for (jsize i = 0; i < plen; i++) {
            unsigned v = (unsigned)ibuf[i];
            out[(size_t)i * 4 + 0] = (uint8_t)((v >> 16) & 0xFF); // R
            out[(size_t)i * 4 + 1] = (uint8_t)((v >> 8) & 0xFF);  // G
            out[(size_t)i * 4 + 2] = (uint8_t)(v & 0xFF);         // B
            out[(size_t)i * 4 + 3] = (uint8_t)((v >> 24) & 0xFF); // A
        }
        ok = true;
    } while (false);
    recycleM = bCls ? env->GetMethodID(bCls, "recycle", "()V") : NULL;
    if (recycleM) {
        if (bmp)    env->CallObjectMethod(bmp, recycleM);
        if (scaled) env->CallObjectMethod(scaled, recycleM);
    }
    if (bmp) env->DeleteLocalRef(bmp);
    if (scaled) env->DeleteLocalRef(scaled);
    if (jarr) env->DeleteLocalRef(jarr);
    if (jintArr) env->DeleteLocalRef(jintArr);
    free(ibuf);
    return ok;
}

// ============================== inject ==============================

static std::set<std::string> g_done;
static bool Done(const std::string& n) { return g_done.count(n) != 0; }
static void MarkDone(const std::string& n) { g_done.insert(n); }

static bool InjectOne(JNIEnv* env, void* tex, const std::string& name) {
    if (!tex || name.empty() || !p_RuntimeInvoke) return false;
    int w = ReadIntInvoke(tex, g_getWM);
    int h = ReadIntInvoke(tex, g_getHM);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return false;

    std::string fname = name;
    for (size_t i = 0; i < fname.size(); i++)
        if (fname[i] == ' ' || fname[i] == '/' || fname[i] == '\\') fname[i] = '_';

    std::vector<uint8_t> png, raw;
    bool havePng = ReadFile(std::string(kTexDir) + "/" + fname + ".png", png);
    bool haveRaw = ReadFile(std::string(kTexDir) + "/" + fname + ".rgba", raw);
    if (!havePng && !haveRaw) return false;

    void* exc = NULL;

    // path 1: ImageConversion.LoadImage(byte[])
    if (havePng && g_loadImageM) {
        void* barr = MakeByteArray(png);
        if (barr) {
            void* args[1] = { barr };
            exc = NULL;
            void* r = p_RuntimeInvoke(g_loadImageM, NULL, args, &exc);
            bool okRes = !exc && r && *(unsigned char*)((char*)r + 0x10) != 0;
            if (okRes && ApplyTex(tex)) { LogF("injected(loadimage): %s", name.c_str()); return true; }
            if (exc) LogF("loadimage exc: %s", name.c_str());
        }
    }

    // path 2: BitmapFactory decode -> LoadRawTextureData(byte[])
    if (havePng && g_loadRawM) {
        std::vector<uint8_t> rgba;
        if (DecodePngToRgba(env, png, w, h, rgba) && rgba.size() == (size_t)w * (size_t)h * 4) {
            void* barr = MakeByteArray(rgba);
            if (barr) {
                void* args[1] = { barr };
                exc = NULL;
                void* r = p_RuntimeInvoke(g_loadRawM, tex, args, &exc);
                bool okRes = !exc && r && *(unsigned char*)((char*)r + 0x10) != 0;
                if (okRes && ApplyTex(tex)) { LogF("injected(raw): %s", name.c_str()); return true; }
                if (exc) LogF("raw exc: %s", name.c_str());
            }
        }
    }

    // path 3: SetPixels32(Color32[])
    if (g_setPix32M && g_color32Class) {
        const std::vector<uint8_t>* pix = NULL;
        std::vector<uint8_t> tmp;
        if (haveRaw && raw.size() == (size_t)w * (size_t)h * 4) pix = &raw;
        else if (havePng && DecodePngToRgba(env, png, w, h, tmp) && tmp.size() == (size_t)w * (size_t)h * 4) pix = &tmp;
        if (pix) {
            void* carr = p_ArrayNew(g_color32Class, (unsigned long)(w * h), NULL);
            if (carr) {
                memcpy((char*)carr + 0x20, pix->data(), pix->size());
                void* args[1] = { carr };
                exc = NULL;
                p_RuntimeInvoke(g_setPix32M, tex, args, &exc);
                if (!exc && ApplyTex(tex)) { LogF("injected(setpixels32): %s", name.c_str()); return true; }
                if (exc) LogF("setpixels32 exc: %s", name.c_str());
            }
        }
    }
    return false;
}

static void RunPass() {
    if (!g_texClass && !ResolveIl2Cpp()) return;
    if (!g_injectEnabled) return;

    // il2cpp requires every native thread to be attached before API calls
    if (p_ThreadAttach && g_domain) p_ThreadAttach(g_domain);

    JNIEnv* env = GetJniEnv();
    if (!env) { static bool warned = false; if (!warned) { LogF("no jni env"); warned = true; } return; }

    if (!g_texTypeObj || !g_findAllM) return;
    void* args1[1] = { g_texTypeObj };
    void* exc = NULL;
    void* arrObj = p_RuntimeInvoke(g_findAllM, NULL, args1, &exc);
    if (exc) { LogF("findAll threw"); return; }
    if (!arrObj) return;

    unsigned count = *(unsigned*)((char*)arrObj + 0x18); // il2cpp array max_length
    if (count > 20000) count = 20000;
    void** items = (void**)((char*)arrObj + 0x20);       // il2cpp array data

    int applied = 0;
    for (unsigned i = 0; i < count; i++) {
        void* tex = items[i];
        if (!tex) continue;
        std::string name = GetTexName(tex);
        if (name.empty()) continue;
        if (Done(name)) continue;
        if (InjectOne(env, tex, name)) { MarkDone(name); applied++; }
    }
    LogF("heartbeat: files=%d applied=%d", CountTexmodFiles(), applied);
}

// ============================== SIGSEGV barrier ==============================
// The crash we chased: our worker thread null-derefs inside libil2cpp
// (fault addr 0x135) and kills the game. The barrier now covers the WHOLE
// worker thread loop, so any such fault is logged and skipped instead.

static __thread sigjmp_buf g_segvJmp;
static __thread bool g_segvActive = false;

static void SegvHandler(int sig, siginfo_t*, void*) {
    if (g_segvActive) siglongjmp(g_segvJmp, 1);
    signal(sig, SIG_DFL); // unrelated crash: restore default and re-raise
    raise(sig);
}

static void InstallSegv() {
    stack_t ss;
    ss.ss_sp = malloc(SIGSTKSZ);
    ss.ss_size = SIGSTKSZ;
    ss.ss_flags = 0;
    if (ss.ss_sp) sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags = SA_ONSTACK | SA_RESTART | SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = SegvHandler;
    sigaction(SIGSEGV, &sa, NULL);
}

// ============================== worker thread ==============================

static void* WorkerThread(void*) {
    InstallSegv();
    LogF("mod start (inject build)");

    {
        FILE* probe = fopen(kLogPath, "a");
        LogF(probe ? "storage access OK" : "storage access FAIL");
        if (probe) fclose(probe);
    }

    int segvStreak = 0;
    while (g_running) {
        bool crashed = false;
        if (sigsetjmp(g_segvJmp, 1) != 0) {
            // jump target: a SIGSEGV inside any il2cpp call of this pass
            g_segvActive = false;
            crashed = true;
            segvStreak++;
            LogF("segv caught in mod thread, pass skipped (%d)", segvStreak);
            if (segvStreak >= 3) {
                LogF("too many segvs, inject passes paused");
                g_injectEnabled = false; // stop hammering a poisoned state
            }
        } else {
            g_segvActive = true;
            RunPass();
            g_segvActive = false;
            segvStreak = 0;
        }
        int waitSec = crashed ? 10 : 5;
        for (int i = 0; i < waitSec && g_running; i++) usleep(100000);
    }
    return NULL;
}

__attribute__((constructor))
static void MainInit() {
    pthread_t t;
    if (pthread_create(&t, NULL, WorkerThread, NULL) == 0) pthread_detach(t);
}

// ============================== mod menu API ==============================

jobjectArray GetFeatureList(JNIEnv* env, jobject context) {
    (void)context;
    if (!env) return NULL;
    jstring sample = env->NewStringUTF("");
    jclass strCls = env->GetObjectClass(sample);
    env->DeleteLocalRef(sample);
    const char* feats[2] = {
        OBFUSCATE("Category_Textures"),
        OBFUSCATE("0_Toggle_Inject Textures"),
    };
    jobjectArray arr = env->NewObjectArray(2, strCls, env->NewStringUTF(""));
    for (int i = 0; i < 2; i++) env->SetObjectArrayElement(arr, i, env->NewStringUTF(feats[i]));
    return arr;
}

void Changes(JNIEnv* env, jclass clazz, jobject obj,
             jint featNum, jstring featName, jint value, jlong Lvalue,
             jboolean boolean, jstring str) {
    (void)clazz; (void)obj; (void)value; (void)Lvalue; (void)str; (void)featNum;
    if (!env || !featName) return;
    const char* fn = env->GetStringUTFChars(featName, NULL);
    bool match = fn && strstr(fn, OBFUSCATE("Inject Textures")) != NULL;
    if (fn) env->ReleaseStringUTFChars(featName, fn);
    if (match) {
        g_injectEnabled = (boolean == JNI_TRUE);
        LogF("inject enabled: %d", (int)g_injectEnabled);
    }
}
