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
#include "Includes/Dobby.h"   // путь поправь под свой шаблон, если отличается
#include "Menu/Menu.hpp"
#include "Menu/Jni.hpp"
#include "Includes/Macros.h"

// ===================== TimeScale: хук set_timeScale =====================

static std::atomic<bool> timescaleOn{false};
static std::atomic<float> timescale{1.0f};
static std::atomic<bool> hookInstalled{false};

typedef void* (*il2cpp_resolve_icall_t)(const char*);
static il2cpp_resolve_icall_t il2cpp_resolve_icall = nullptr;

typedef void (*set_timeScale_t)(float);
static set_timeScale_t old_set_timeScale = nullptr;

// Хук: пока тумблер включён — любое обращение игры к set_timeScale
// заменяется нашим значением. Выключен — игра живёт своей жизнью.
static void _set_timeScale(float value) {
    if (!old_set_timeScale) return;
    if (timescaleOn.load())
        old_set_timeScale(timescale.load());
    else
        old_set_timeScale(value);
}

static void InstallTimeScaleHook() {
    void *handle = dlopen("libil2cpp.so", RTLD_NOW | RTLD_GLOBAL);
    if (!handle) return;

    il2cpp_resolve_icall = (il2cpp_resolve_icall_t) dlsym(handle, "il2cpp_resolve_icall");
    if (!il2cpp_resolve_icall) return;

    void *fn = il2cpp_resolve_icall(OBFUSCATE("UnityEngine.Time::set_timeScale"));
    if (!fn) {
        LOGI(OBFUSCATE("set_timeScale not found"));
        return;
    }

    DobbyHook(fn, (void *) _set_timeScale, (void **) &old_set_timeScale);
    hookInstalled = true;
    LOGI(OBFUSCATE("TimeScale hook installed"));
}

// Поддержка: если игра редко трогает timeScale — периодически применяем сами
static void TimeScaleKeepAlive() {
    while (true) {
        if (timescaleOn.load() && hookInstalled.load() && old_set_timeScale)
            old_set_timeScale(timescale.load());
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    }
}

// ================================================================================

// Do not change or translate the first text unless you know what you are doing
jobjectArray GetFeatureList(JNIEnv *env, jobject context) {
    jobjectArray ret;

    // Динамическая подпись: показывает текущее значение timescale
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
            if (!timescaleOn && hookInstalled.load() && old_set_timeScale) {
                timescale = 1.0f;
                old_set_timeScale(1.0f);   // off(): возврат к нормальной скорости
            }
            break;

        case 1: // SeekBar: 1..100 -> 0.1..10.0
            timescale = value / 10.0f;
            timescaleOn = true;
            if (hookInstalled.load() && old_set_timeScale)
                old_set_timeScale(timescale.load());
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
                timescale = v;      // любое значение, включая минус и дроби
                timescaleOn = true; // ввод значения = тумблер включён
                if (hookInstalled.load() && old_set_timeScale)
                    old_set_timeScale(v);
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

    InstallTimeScaleHook();
    std::thread(TimeScaleKeepAlive).detach();

    LOGI(OBFUSCATE("Done"));
}

__attribute__((constructor))
void lib_main() {
    std::thread(hack_thread).detach();
}
