#include <list>
#include <vector>
#include <cstring>
#include <pthread.h>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <string>
#include <algorithm>
#include <cstdio>
#include <jni.h>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <dlfcn.h>
#include "Includes/Logger.h"
#include "Includes/obfuscate.h"
#include "Includes/Utils.hpp"
#include "Menu/Menu.hpp"
#include "Menu/Jni.hpp"
#include "Includes/Macros.h"

// ===================== TimeScale: прямая запись в статическое поле =====================

static std::atomic<bool> timescaleOn{false};
static std::atomic<float> timescale{1.0f};

// il2cpp API
typedef void* (*il2cpp_domain_get_t)();
typedef void** (*il2cpp_domain_get_assemblies_t)(void*, size_t*);
typedef void* (*il2cpp_assembly_get_image_t)(void*);
typedef void* (*il2cpp_class_from_name_t)(void*, const char*, const char*);
typedef void* (*il2cpp_class_get_field_from_name_t)(void*, const char*);
typedef void  (*il2cpp_field_static_set_value_t)(void*, void*);
typedef void  (*il2cpp_field_static_get_value_t)(void*, void*);

static il2cpp_domain_get_t              il2cpp_domain_get = nullptr;
static il2cpp_domain_get_assemblies_t   il2cpp_domain_get_assemblies = nullptr;
static il2cpp_assembly_get_image_t      il2cpp_assembly_get_image = nullptr;
static il2cpp_class_from_name_t         il2cpp_class_from_name = nullptr;
static il2cpp_class_get_field_from_name_t il2cpp_class_get_field_from_name = nullptr;
static il2cpp_field_static_set_value_t  il2cpp_field_static_set_value = nullptr;
static il2cpp_field_static_get_value_t  il2cpp_field_static_get_value = nullptr;

static void* timeScaleField = nullptr; // FieldInfo* UnityEngine.Time::timeScale

static bool ResolveIl2Cpp() {
    void *handle = dlopen("libil2cpp.so", RTLD_NOW | RTLD_GLOBAL);
    if (!handle) return false;

    il2cpp_domain_get              = (il2cpp_domain_get_t) dlsym(handle, "il2cpp_domain_get");
    il2cpp_domain_get_assemblies   = (il2cpp_domain_get_assemblies_t) dlsym(handle, "il2cpp_domain_get_assemblies");
    il2cpp_assembly_get_image      = (il2cpp_assembly_get_image_t) dlsym(handle, "il2cpp_assembly_get_image");
    il2cpp_class_from_name         = (il2cpp_class_from_name_t) dlsym(handle, "il2cpp_class_from_name");
    il2cpp_class_get_field_from_name = (il2cpp_class_get_field_from_name_t) dlsym(handle, "il2cpp_class_get_field_from_name");
    il2cpp_field_static_set_value  = (il2cpp_field_static_set_value_t) dlsym(handle, "il2cpp_field_static_set_value");
    il2cpp_field_static_get_value  = (il2cpp_field_static_get_value_t) dlsym(handle, "il2cpp_field_static_get_value");

    if (!il2cpp_domain_get || !il2cpp_domain_get_assemblies || !il2cpp_assembly_get_image ||
        !il2cpp_class_from_name || !il2cpp_class_get_field_from_name ||
        !il2cpp_field_static_set_value || !il2cpp_field_static_get_value)
        return false;

    // Ищем класс UnityEngine.Time среди всех сборок
    void *domain = il2cpp_domain_get();
    if (!domain) return false;

    size_t count = 0;
    void **assemblies = il2cpp_domain_get_assemblies(domain, &count);
    if (!assemblies) return false;

    for (size_t i = 0; i < count; i++) {
        void *image = il2cpp_assembly_get_image(assemblies[i]);
        if (!image) continue;
        void *klass = il2cpp_class_from_name(image, OBFUSCATE("UnityEngine"), OBFUSCATE("Time"));
        if (!klass) continue;

        timeScaleField = il2cpp_class_get_field_from_name(klass, OBFUSCATE("timeScale"));
        if (timeScaleField) return true;
    }
    return false;
}

// Пишем в поле напрямую: никаких clamp-ов, минус проходит как есть
static void ApplyTimeScale(float v) {
    if (timeScaleField && il2cpp_field_static_set_value)
        il2cpp_field_static_set_value(timeScaleField, &v);
}

// Фоновый поток: пока тумблер включён — держим наше значение в поле,
// даже если игра сама перезапишет timeScale через обычный сеттер.
static void TimeScaleKeepAlive() {
    while (!timeScaleField) {
        ResolveIl2Cpp();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    while (true) {
        if (timescaleOn)
            ApplyTimeScale(timescale.load());
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    }
}

// ================================================================================

// Do not change or translate the first text unless you know what you are doing
jobjectArray GetFeatureList(JNIEnv *env, jobject context) {
    jobjectArray ret;

    // Динамическая подпись: показывает текущее значение timescale,
    // включая вручную заданное (минус и дроби)
    char customLabel[64];
    snprintf(customLabel, sizeof(customLabel),
             "2_InputText_Custom speed (now: %.2f)", timescale.load());

    const char *features[] = {
            OBFUSCATE("Category_TimeScale"),                 // не нумеруется
            OBFUSCATE("0_Toggle_TimeScale enabled"),         // case 0
            OBFUSCATE("1_SeekBar_Speed x0.1-x10_1_100"),     // case 1 (значение /10)
            customLabel                                      // case 2
    };

    int Total_Feature = (sizeof features / sizeof features[0]);
    ret = (jobjectArray)
            env->NewObjectArray(Total_Feature, env->FindClass(OBFUSCATE("java/lang/String")),
                                env->NewStringUTF(""));

    for (int i = 0; i < Total_Feature; i++)
        env->SetObjectArrayElement(ret, i, env->NewStringUTF(features[i]));

    return (ret);
}

#define targetLibName OBFUSCATE("libil2cpp.so")

void Changes(JNIEnv *env, jclass clazz, jobject obj, jint featNum, jstring featName,
             jint value, jlong Lvalue, jboolean boolean, jstring text) {

    switch (featNum) {
        case 0: // Toggle_TimeScale enabled
            timescaleOn = boolean;
            if (!timescaleOn)
                ApplyTimeScale(1.0f);   // off(): возврат к нормальной скорости
            break;

        case 1: // SeekBar: 1..100 -> 0.1..10.0
            timescale = value / 10.0f;
            ApplyTimeScale(timescale.load());
            break;

        case 2: { // InputText: ручной ввод без ограничений
            if (!text) break;
            const char *str = env->GetStringUTFChars(text, nullptr);
            if (str) {
                std::string s(str);
                env->ReleaseStringUTFChars(text, str);

                // русская раскладка вводит "0,5" — приводим к "0.5"
                std::replace(s.begin(), s.end(), ',', '.');

                float v = strtof(s.c_str(), nullptr);
                timescale = v;      // любое значение: минус, дробь, без ограничений
                timescaleOn = true; // ввод значения = тумблер включён
                ApplyTimeScale(timescale.load());
            }
            break;
        }

        default:
            break;
    }
}

void hack_thread() {
    // libil2cpp.so загружается не сразу — ждём
    while (!isLibraryLoaded(targetLibName)) {
        sleep(1);
    }

    ResolveIl2Cpp();
    std::thread(TimeScaleKeepAlive).detach();

    LOGI(OBFUSCATE("Done"));
}

__attribute__((constructor))
void lib_main() {
    std::thread(hack_thread).detach();
}
