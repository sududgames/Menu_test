// Textur_moder — Main.cpp (full fix build)
#include <jni.h>
#include <android/log.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <unistd.h>
#include <dlfcn.h>
#include <dirent.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include "Menu/Menu.hpp"

#define LOG_PATH "/storage/emulated/0/texmod_log.txt"

static pthread_mutex_t g_logMtx = PTHREAD_MUTEX_INITIALIZER;
static char g_logBuf[8192];
static size_t g_logLen = 0;
static bool g_logReady = false;

static void LogMsg(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    pthread_mutex_lock(&g_logMtx);
    if (!g_logReady) {
        size_t need = (size_t)n + 1;
        if (g_logLen + need <= sizeof(g_logBuf)) {
            memcpy(g_logBuf + g_logLen, line, need);
            g_logLen += need - 1;
            g_logBuf[g_logLen++] = '\n';
        }
    } else {
        FILE* f = fopen(LOG_PATH, "a");
        if (f) {
            if (g_logLen) {
                fwrite(g_logBuf, 1, g_logLen, f);
                g_logLen = 0;
            }
            fprintf(f, "%s\n", line);
            fclose(f);
        }
    }
    pthread_mutex_unlock(&g_logMtx);
}

static void FlushEarlyLog() {
    pthread_mutex_lock(&g_logMtx);
    FILE* f = fopen(LOG_PATH, "a");
    if (f) {
        fprintf(f, "===== launch =====\n");
        if (g_logLen) {
            fwrite(g_logBuf, 1, g_logLen, f);
            g_logLen = 0;
        }
        fclose(f);
        g_logReady = true;
    }
    pthread_mutex_unlock(&g_logMtx);
}

static void* g_il2cppHandle = NULL;
static void* g_domain = NULL;
static bool g_injectEnabled = true;

typedef void* (*fn_DomainGet)();
typedef void** (*fn_DomainGetAssemblies)(void*, unsigned long long*);
typedef void* (*fn_AsmGetImage)(void*);
typedef const char* (*fn_ImageGetName)(void*);
typedef void* (*fn_ClassFromName)(void*, const char*, const char*);
typedef void* (*fn_ClassGetMethod)(void*, const char*, int);
typedef void* (*fn_ClassGetField)(void*, const char*);
typedef void* (*fn_FieldGetValue)(void*, void*, void*);
typedef void* (*fn_ClassGetType)(void*);
typedef void* (*fn_TypeGetObject)(void*);
typedef void* (*fn_RuntimeInvoke)(const void*, void*, void**, void**);
typedef void* (*fn_ResolveIcall)(const char*);
typedef void* (*fn_ArrayNew)(void*, unsigned long, void*);
typedef void* (*fn_ThreadAttach)(void*);
typedef void* (*fn_ObjectGetClass)(void*);
typedef void* (*fn_ClassGetParent)(void*);

static fn_DomainGet           p_DomainGet           = NULL;
static fn_DomainGetAssemblies p_DomainGetAssemblies = NULL;
static fn_AsmGetImage         p_AsmGetImage         = NULL;
static fn_ImageGetName        p_ImageGetName        = NULL;
static fn_ClassFromName       p_ClassFromName       = NULL;
static fn_ClassGetMethod      p_ClassGetMethod      = NULL;
static fn_ClassGetField       p_FieldFromName       = NULL;
static fn_FieldGetValue       p_FieldGetValue       = NULL;
static fn_ClassGetType        p_ClassGetType        = NULL;
static fn_TypeGetObject       p_TypeGetObject       = NULL;
static fn_RuntimeInvoke       p_RuntimeInvoke       = NULL;
static fn_ResolveIcall        p_ResolveIcall        = NULL;
static fn_ArrayNew            p_ArrayNew            = NULL;
static fn_ThreadAttach        p_ThreadAttach        = NULL;
static fn_ObjectGetClass      p_ObjectGetClass      = NULL;
static fn_ClassGetParent      p_ClassGetParent      = NULL;

#define GETSYM(var, name) if (!(var)) (var) = reinterpret_cast<decltype(var)>(dlsym(g_il2cppHandle, name));

static void* g_texClass  = NULL;
static void* g_resClass  = NULL;
static void* g_coreClass = NULL;
static void* g_byteClass = NULL;
static void* g_typeObj   = NULL;
static void* g_findAll   = NULL;
static void* g_getName   = NULL;
static void* g_apply0    = NULL;
static void* g_loadImage = NULL;
static void* g_loadRaw   = NULL;
static void* g_setPix32  = NULL;
static void* g_nameField = NULL;
static void* g_wField    = NULL;
static void* g_hField    = NULL;

static JavaVM* g_jvm = NULL;

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    g_jvm = vm;
    return JNI_VERSION_1_6;
}

static JNIEnv* GetJniEnv() {
    if (!g_jvm) {
        void* sym = dlsym(RTLD_DEFAULT, "_ZN7android14AndroidRuntime7mJavaVME");
        if (sym && *(void**)sym) g_jvm = (JavaVM*)*(void**)sym;
    }
    if (!g_jvm) return NULL;
    JNIEnv* env = NULL;
    if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK && env) return env;
    if (g_jvm->AttachCurrentThread(&env, NULL) == JNI_OK && env) return env;
    return NULL;
}

static std::string Utf16ToUtf8(void* str) {
    if (!str) return "";
    unsigned len = *(unsigned*)((char*)str + sizeof(void*) * 2);
    if (len == 0 || len > 8192) return "";
    const char16_t* chars = (const char16_t*)((char*)str + sizeof(void*) * 2 + 4);
    std::string out;
    out.reserve(len);
    for (unsigned i = 0; i < len; i++) {
        unsigned c = (unsigned)chars[i];
        if (c < 0x80) {
            out.push_back((char)c);
        } else if (c < 0x800) {
            out.push_back((char)(0xC0 | (c >> 6)));
            out.push_back((char)(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back((char)(0xE0 | (c >> 12)));
            out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (c & 0x3F)));
        } else {
            out.push_back('?');
        }
    }
    return out;
}

static std::string SpaceToUnderscore(const std::string& in) {
    std::string out = in;
    for (size_t i = 0; i < out.size(); i++)
        if (out[i] == ' ') out[i] = '_';
    return out;
}

static std::string GetTexName(void* tex) {
    if (g_nameField) {
        void* str = NULL;
        p_FieldGetValue(tex, g_nameField, &str);
        std::string s = Utf16ToUtf8(str);
        if (!s.empty()) return s;
    }
    if (g_getName) {
        void* exc = NULL;
        void* str = p_RuntimeInvoke(g_getName, tex, NULL, &exc);
        if (!exc && str) return Utf16ToUtf8(str);
    }
    return "";
}

static bool IsTexture2D(void* obj) {
    if (!obj || !p_ObjectGetClass || !g_texClass) return false;
    void* cls = p_ObjectGetClass(obj);
    for (int depth = 0; cls && depth < 8; depth++) {
        if (cls == g_texClass) return true;
        if (!p_ClassGetParent) break;
        cls = p_ClassGetParent(cls);
    }
    return false;
}

static __thread sigjmp_buf g_segvJmp;
static __thread volatile int g_segvActive = 0;
static struct sigaction g_oldSegv;
static volatile int g_segvInstalled = 0;

static void SegvHandler(int) {
    if (g_segvActive) siglongjmp(g_segvJmp, 1);
    _exit(0);
}

__attribute__((noinline))
static bool BeginGuard() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = SegvHandler;
    if (sigaction(SIGSEGV, &sa, &g_oldSegv) != 0) return false;
    g_segvInstalled = 1;
    if (sigsetjmp(g_segvJmp, 1) != 0) {
        g_segvActive = 0;
        sigaction(SIGSEGV, &g_oldSegv, NULL);
        g_segvInstalled = 0;
        return false;
    }
    g_segvActive = 1;
    return true;
}

static void EndGuard() {
    g_segvActive = 0;
    if (g_segvInstalled) {
        sigaction(SIGSEGV, &g_oldSegv, NULL);
        g_segvInstalled = 0;
    }
}

static bool DecodePngToRgba(const unsigned char* data, size_t len, int tw, int th,
                            std::vector<unsigned char>& out) {
    JNIEnv* env = GetJniEnv();
    if (!env || len < 8) return false;

    bool ok = false;
    jobject bmp = NULL, scaled = NULL;
    jbyteArray jarr = NULL;
    jintArray jpix = NULL;
    jint* raw = NULL;
    jmethodID gw = NULL, gh = NULL, getpix = NULL, recycle = NULL, scale = NULL, decode = NULL;
    jclass bmpCls = NULL, bmfCls = NULL, bmpReal = NULL;

    do {
        bmfCls = env->FindClass("android/graphics/BitmapFactory");
        if (!bmfCls || env->ExceptionCheck()) { if (env->ExceptionCheck()) env->ExceptionClear(); break; }
        bmpCls = env->FindClass("android/graphics/Bitmap");
        if (!bmpCls || env->ExceptionCheck()) { if (env->ExceptionCheck()) env->ExceptionClear(); break; }

        jarr = env->NewByteArray((jsize)len);
        if (!jarr) break;
        env->SetByteArrayRegion(jarr, 0, (jsize)len, (const jbyte*)data);
        decode = env->GetStaticMethodID(bmfCls, "decodeByteArray", "([BII)Landroid/graphics/Bitmap;");
        if (!decode) break;
        bmp = env->CallStaticObjectMethod(bmfCls, decode, jarr, 0, (jint)len);
        if (env->ExceptionCheck()) { env->ExceptionClear(); bmp = NULL; }
        if (!bmp) break;

        bmpReal = env->GetObjectClass(bmp);
        gw      = env->GetMethodID(bmpReal, "getWidth", "()I");
        gh      = env->GetMethodID(bmpReal, "getHeight", "()I");
        getpix  = env->GetMethodID(bmpReal, "getPixels", "([IIIIIII)V");
        recycle = env->GetMethodID(bmpReal, "recycle", "()V");
        scale   = env->GetStaticMethodID(bmpReal, "createScaledBitmap",
                                         "(Landroid/graphics/Bitmap;IIZ)Landroid/graphics/Bitmap;");
        if (!gw || !gh || !getpix || !recycle) break;

        jint w = env->CallIntMethod(bmp, gw);
        jint h = env->CallIntMethod(bmp, gh);
        if (w <= 0 || h <= 0) break;

        if (tw > 0 && th > 0 && (w != tw || h != th)) {
            if (scale) {
                scaled = env->CallStaticObjectMethod(bmpReal, scale, bmp, tw, th, JNI_FALSE);
                if (env->ExceptionCheck()) { env->ExceptionClear(); scaled = NULL; }
                if (scaled) {
                    env->CallVoidMethod(bmp, recycle);
                    bmp = scaled;
                    scaled = NULL;
                    w = tw;
                    h = th;
                }
            }
            if (w != tw || h != th) break;
        }

        jpix = env->NewIntArray(w * h);
        if (!jpix) break;
        env->CallVoidMethod(bmp, getpix, jpix, 0, w, 0, 0, w, h);
        if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
        raw = env->GetIntArrayElements(jpix, NULL);
        if (!raw) break;

        out.resize((size_t)w * h * 4);
        for (int i = 0; i < w * h; i++) {
            unsigned p = (unsigned)raw[i];
            out[i * 4 + 0] = (unsigned char)(p >> 16);
            out[i * 4 + 1] = (unsigned char)(p >> 8);
            out[i * 4 + 2] = (unsigned char)(p);
            out[i * 4 + 3] = (unsigned char)(p >> 24);
        }
        ok = true;
    } while (false);

    if (raw) env->ReleaseIntArrayElements(jpix, raw, JNI_ABORT);
    if (bmp && recycle) env->CallVoidMethod(bmp, recycle);
    if (jpix) env->DeleteLocalRef(jpix);
    if (jarr) env->DeleteLocalRef(jarr);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return ok;
}

struct FileEntry {
    std::string name;
    std::vector<unsigned char> data;
    bool isRgba;
};

static std::vector<FileEntry> LoadTexFiles() {
    std::vector<FileEntry> out;
    const char* dir = "/storage/emulated/0/texmod";
    DIR* d = opendir(dir);
    if (!d) return out;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        std::string fn = e->d_name;
        if (fn == "." || fn == "..") continue;
        std::string lower = fn;
        for (size_t i = 0; i < lower.size(); i++)
            lower[i] = (char)tolower((unsigned char)lower[i]);
        bool isPng  = lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".png") == 0;
        bool isRgba = lower.size() > 5 && lower.compare(lower.size() - 5, 5, ".rgba") == 0;
        if (!isPng && !isRgba) continue;
        std::string full = std::string(dir) + "/" + fn;
        FILE* f = fopen(full.c_str(), "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz <= 64 * 1024 * 1024) {
            FileEntry fe;
            fe.name = fn.substr(0, fn.rfind('.'));
            fe.isRgba = isRgba;
            fe.data.resize((size_t)sz);
            size_t rd = fread(&fe.data[0], 1, (size_t)sz, f);
            if (rd == (size_t)sz) out.push_back(fe);
        }
        fclose(f);
    }
    closedir(d);
    return out;
}

static void* NewByteArrayObj(unsigned long size) {
    if (!g_byteClass || !p_ArrayNew) return NULL;
    return p_ArrayNew(g_byteClass, size, NULL);
}

static void* NewColor32ArrayObj(unsigned long count) {
    if (!g_coreClass || !p_ArrayNew) return NULL;
    return p_ArrayNew(g_coreClass, count, NULL);
}

static bool PushRawToTexture(void* tex, const std::vector<unsigned char>& rgba) {
    if (!g_loadRaw) return false;
    void* jarr = NewByteArrayObj((unsigned long)rgba.size());
    if (!jarr) return false;
    memcpy((char*)jarr + sizeof(void*) * 4, rgba.data(), rgba.size());
    void* args[1] = { jarr };
    void* exc = NULL;
    p_RuntimeInvoke(g_loadRaw, tex, args, &exc);
    if (exc) return false;
    if (!g_apply0) return false;
    exc = NULL;
    p_RuntimeInvoke(g_apply0, tex, NULL, &exc);
    return !exc;
}

static bool PushPixels32(void* tex, const std::vector<unsigned char>& rgba) {
    if (!g_setPix32 || !g_coreClass) return false;
    unsigned long count = rgba.size() / 4;
    void* arr = NewColor32ArrayObj(count);
    if (!arr) return false;
    memcpy((char*)arr + sizeof(void*) * 4, rgba.data(), rgba.size());
    void* args[1] = { arr };
    void* exc = NULL;
    p_RuntimeInvoke(g_setPix32, tex, args, &exc);
    if (exc) return false;
    if (!g_apply0) return false;
    exc = NULL;
    p_RuntimeInvoke(g_apply0, tex, NULL, &exc);
    return !exc;
}

static void TryInjectTexture(void* tex, const std::string& name,
                             const std::vector<unsigned char>& data, bool isRgba) {
    int w = 0, h = 0;
    if (g_wField) p_FieldGetValue(tex, g_wField, &w);
    if (g_hField) p_FieldGetValue(tex, g_hField, &h);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return;
    size_t need = (size_t)w * (size_t)h * 4;

    std::vector<unsigned char> rgba;
    if (isRgba) {
        if (data.size() != need) return;
        rgba = data;
    } else {
        if (!DecodePngToRgba(data.data(), data.size(), w, h, rgba)) {
            LogMsg("png decode failed: %s", name.c_str());
            return;
        }
    }
    if (rgba.size() != need) return;

    if (g_loadImage) {
        void* jarr = NewByteArrayObj((unsigned long)data.size());
        if (jarr) {
            memcpy((char*)jarr + sizeof(void*) * 4, data.data(), data.size());
            unsigned char one = 1;
            void* args[3] = { tex, jarr, &one };
            void* exc = NULL;
            p_RuntimeInvoke(g_loadImage, NULL, args, &exc);
            if (!exc) {
                LogMsg("injected: %s", name.c_str());
                return;
            }
        }
    }

    if (PushRawToTexture(tex, rgba)) {
        LogMsg("injected: %s", name.c_str());
        return;
    }

    if (PushPixels32(tex, rgba)) {
        LogMsg("injected: %s", name.c_str());
        return;
    }

    LogMsg("no inject path for %s", name.c_str());
}

static void InjectPass() {
    if (!g_injectEnabled) return;
    if (!BeginGuard()) {
        LogMsg("segv caught in inject pass, skipped");
        return;
    }

    std::vector<FileEntry> files = LoadTexFiles();
    if (files.empty()) {
        EndGuard();
        return;
    }

    if (!g_findAll || !g_typeObj) {
        EndGuard();
        return;
    }

    void* exc = NULL;
    void* args[1] = { g_typeObj };
    void* arr = p_RuntimeInvoke(g_findAll, NULL, args, &exc);
    if (exc || !arr) {
        EndGuard();
        LogMsg("findAll failed");
        return;
    }

    unsigned long n = *(unsigned long*)((char*)arr + sizeof(void*) * 3);
    void** items = (void**)((char*)arr + sizeof(void*) * 4);
    int applied = 0;

    for (unsigned long i = 0; i < n && i < 200000; i++) {
        void* obj = items[i];
        if (!obj || !IsTexture2D(obj)) continue;
        std::string name = GetTexName(obj);
        if (name.empty()) continue;
        std::string key = SpaceToUnderscore(name);
        for (size_t fi = 0; fi < files.size(); fi++) {
            if (files[fi].name != key) continue;
            TryInjectTexture(obj, name, files[fi].data, files[fi].isRgba);
            applied++;
            break;
        }
    }

    LogMsg("heartbeat: files=%d applied=%d", (int)files.size(), applied);
    EndGuard();
}

static void* FindClassInAssemblies(const char* ns, const char* name) {
    if (!g_domain || !p_DomainGetAssemblies || !p_ClassFromName) return NULL;
    unsigned long long n = 0;
    void** assemblies = p_DomainGetAssemblies(g_domain, &n);
    if (!assemblies) return NULL;
    for (unsigned long long i = 0; i < n; i++) {
        if (!assemblies[i] || !p_AsmGetImage) continue;
        void* img = p_AsmGetImage(assemblies[i]);
        if (!img || !p_ImageGetName) continue;
        const char* iname = p_ImageGetName(img);
        if (!iname) continue;
        void* cls = p_ClassFromName(img, ns, name);
        if (cls) return cls;
    }
    return NULL;
}

static void ResolveIl2Cpp() {
    GETSYM(p_DomainGet,           "il2cpp_domain_get");
    GETSYM(p_DomainGetAssemblies, "il2cpp_domain_get_assemblies");
    GETSYM(p_AsmGetImage,         "il2cpp_assembly_get_image");
    GETSYM(p_ImageGetName,        "il2cpp_image_get_name");
    GETSYM(p_ClassFromName,       "il2cpp_class_from_name");
    GETSYM(p_ClassGetMethod,      "il2cpp_class_get_method_from_name");
    GETSYM(p_FieldFromName,       "il2cpp_class_get_field_from_name");
    GETSYM(p_FieldGetValue,       "il2cpp_field_get_value");
    GETSYM(p_ClassGetType,        "il2cpp_class_get_type");
    GETSYM(p_TypeGetObject,       "il2cpp_type_get_object");
    GETSYM(p_RuntimeInvoke,       "il2cpp_runtime_invoke");
    GETSYM(p_ResolveIcall,        "il2cpp_resolve_icall");
    GETSYM(p_ArrayNew,            "il2cpp_array_new");
    GETSYM(p_ThreadAttach,        "il2cpp_thread_attach");
    GETSYM(p_ObjectGetClass,      "il2cpp_object_get_class");
    GETSYM(p_ClassGetParent,      "il2cpp_class_get_parent");

    if (!p_DomainGet || !p_DomainGetAssemblies) {
        LogMsg("il2cpp api missing");
        return;
    }

    for (int tries = 0; tries < 120; tries++) {
        g_domain = p_DomainGet();
        if (g_domain) break;
        usleep(500000);
    }
    if (!g_domain) {
        LogMsg("no domain");
        return;
    }
    LogMsg("il2cpp domain ready");

    if (p_ThreadAttach) p_ThreadAttach(g_domain);

    unsigned long long n = 0, prev = 0;
    void** assemblies = NULL;
    for (int tries = 0; tries < 120; tries++) {
        assemblies = p_DomainGetAssemblies(g_domain, &n);
        if (assemblies && n > 0 && n == prev) break;
        prev = n;
        usleep(500000);
    }
    if (!assemblies || n == 0) {
        LogMsg("no assemblies");
        return;
    }
    for (unsigned long long i = 0; i < n; i++) {
        if (!assemblies[i] || !p_AsmGetImage || !p_ImageGetName) continue;
        void* img = p_AsmGetImage(assemblies[i]);
        if (!img) continue;
        const char* iname = p_ImageGetName(img);
        LogMsg("asm[%d]: %s", (int)i, iname ? iname : "?");
    }

    g_texClass  = FindClassInAssemblies("UnityEngine", "Texture2D");
    g_resClass  = FindClassInAssemblies("UnityEngine", "Resources");
    g_coreClass = FindClassInAssemblies("UnityEngine", "Color32");
    g_byteClass = FindClassInAssemblies("System", "Byte");

    if (!g_texClass) {
        LogMsg("tex class missing");
        return;
    }

    void* type = p_ClassGetType ? p_ClassGetType(g_texClass) : NULL;
    g_typeObj = (type && p_TypeGetObject) ? p_TypeGetObject(type) : NULL;

    if (g_resClass && p_ClassGetMethod)
        g_findAll = p_ClassGetMethod(g_resClass, "FindObjectsOfTypeAll", 1);
    if (p_ClassGetMethod) {
        g_getName  = p_ClassGetMethod(g_texClass, "GetName", 0);
        g_apply0   = p_ClassGetMethod(g_texClass, "Apply", 1);
        g_loadRaw  = p_ClassGetMethod(g_texClass, "LoadRawTextureData", 1);
        g_setPix32 = p_ClassGetMethod(g_texClass, "SetPixels32", 1);
        void* imgConv = FindClassInAssemblies("UnityEngine", "ImageConversion");
        if (imgConv) g_loadImage = p_ClassGetMethod(imgConv, "LoadImage", 3);
    }
    if (p_FieldFromName) {
        g_nameField = p_FieldFromName(g_texClass, "m_Name");
        g_wField    = p_FieldFromName(g_texClass, "m_Width");
        g_hField    = p_FieldFromName(g_texClass, "m_Height");
    }

    LogMsg("resolved: tex=%p res=%p imgConv=%p findAll=%p getName=%p apply0=%p load3=%p set32=%p icall=%p",
           g_texClass, g_resClass, g_loadImage, g_findAll, g_getName,
           g_apply0, g_loadRaw, g_setPix32, p_ResolveIcall);
    LogMsg("fallback: type=%p nameField=%p wField=%p hField=%p",
           g_typeObj, g_nameField, g_wField, g_hField);
}

static void* ModThread(void*) {
    sleep(5);
    FlushEarlyLog();
    LogMsg("mod start");
    ResolveIl2Cpp();
    while (true) {
        if (g_injectEnabled) InjectPass();
        sleep(10);
    }
    return NULL;
}

__attribute__((constructor(101)))
static void ModInit() {
    pthread_t t;
    if (pthread_create(&t, NULL, ModThread, NULL) == 0)
        pthread_detach(t);
}

jobjectArray GetFeatureList(JNIEnv *env, jobject context) {
    (void)context;
    const char* features[] = {
        "Category_Textures",
        "0_Toggle_Inject Textures",
    };
    int total = (int)(sizeof(features) / sizeof(features[0]));

    jclass strCls = env->FindClass("java/lang/String");
    if (!strCls || env->ExceptionCheck()) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        strCls = env->GetObjectClass(env->NewStringUTF(""));
    }
    jobjectArray ret = env->NewObjectArray(total, strCls, env->NewStringUTF(""));
    for (int i = 0; i < total; i++)
        env->SetObjectArrayElement(ret, i, env->NewStringUTF(features[i]));
    return ret;
}

void Changes(JNIEnv *env, jclass clazz, jobject obj,
             jint featNum, jstring featName, jint value, jlong Lvalue,
             jboolean boolean, jstring str) {
    (void)env; (void)clazz; (void)obj; (void)featName;
    (void)value; (void)Lvalue; (void)str;
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (featNum == 0) {
        g_injectEnabled = (boolean == JNI_TRUE);
        LogMsg("inject enabled: %d", (int)(boolean == JNI_TRUE));
    }
}
