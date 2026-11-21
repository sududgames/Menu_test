#include <list>
#include <vector>
#include <cstring>
#include <pthread.h>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <string>
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
#include "dobby.h"

// ===================== TimeScale (перенос из Frida-скрипта) =====================

static std::atomic<bool> timescaleOn{false};
static std::atomic<float> timescale{1.0f};

typedef void (*set_timeScale_t)(float);
typedef float (*get_timeScale_t)();

static set_timeScale_t set_timeScale = nullptr;
static get_timeScale_t get_timeScale = nullptr;

static void ResolveTimeScale() {
    void *handle = dlopen("libil2cpp.so", RTLD_NOW | RTLD_GLOBAL);
    if (!handle) return;

    auto resolve = (void* (*)(const char*)) dlsym(handle, "il2cpp_resolve_icall");
    if (!resolve) return;

    set_timeScale = (set_timeScale_t) resolve(OBFUSCATE("UnityEngine.Time::set_timeScale"));
    get_timeScale = (get_timeScale_t) resolve(OBFUSCATE("UnityEngine.Time::get_timeScale"));
}

// Фоновый поток: держит timeScale, пока включён тумблер (аналог setInterval из скрипта)
static void TimeScaleKeepAlive() {
    while (!set_timeScale) {
        ResolveTimeScale();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    while (true) {
        if (timescaleOn && set_timeScale)
            set_timeScale(timescale);
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    }
}

// ================================================================================

// Do not change or translate the first text unless you know what you are doing
jobjectArray GetFeatureList(JNIEnv *env, jobject context) {
    jobjectArray ret;

    const char *features[] = {
            OBFUSCATE("Category_TimeScale"),                          // не нумеруется
            OBFUSCATE("0_Toggle_TimeScale enabled"),                  // case 0
            OBFUSCATE("1_SeekBar_Speed x0.1-x10_1_100"),              // case 1 (значение /10)
            OBFUSCATE("2_InputText_Custom speed (e.g. -0.75 or 2.5)") // case 2
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
            if (!timescaleOn && set_timeScale)
                set_timeScale(1.0f);   // off(): возврат к нормальной скорости
            break;

        case 1: // SeekBar: 1..100 -> 0.1..10.0
            timescale = value / 10.0f;
            break;

        case 2: { // InputText: ручной ввод с минусом и дробью
            if (!text) break;
            const char *str = env->GetStringUTFChars(text, nullptr);
            if (str) {
                float v = strtof(str, nullptr);
                env->ReleaseStringUTFChars(text, str);

                if (v >= -100.0f && v <= 100.0f)  // защита от случайного 99999
                    timescale = v;
                // невалидный/пустой ввод игнорируется — скорость не меняется
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

    // TimeScale: резолв icall-ов + фоновый поток поддержки
    ResolveTimeScale();
    std::thread(TimeScaleKeepAlive).detach();

    LOGI(OBFUSCATE("Done"));
}

__attribute__((constructor))
void lib_main() {
    std::thread(hack_thread).detach();
}
