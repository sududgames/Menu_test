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
static get_timeScale_t orig_get_timeScale = nullptr;

// Хук геттера: пока тумблер включён — отдаём наше значение всем читателям
// (и managed-коду, и нативному движку), включая отрицательные.
// Так мы обходим нативный сеттер, который режет минус до нуля.
static float hooked_get_timeScale() {
    if (timescaleOn.load(std::memory_order_relaxed))
        return timescale.load(std::memory_order_relaxed);
    return orig_get_timeScale();
}

static void ResolveTimeScale() {
    void *handle = dlopen("libil2cpp.so", RTLD_NOW | RTLD_GLOBAL);
    if (!handle) return;

    auto resolve = (void* (*)(const char*)) dlsym(handle, "il2cpp_resolve_icall");
    if (!resolve) return;

    set_timeScale = (set_timeScale_t) resolve(OBFUSCATE("UnityEngine.Time::set_timeScale"));
    get_timeScale = (get_timeScale_t) resolve(OBFUSCATE("UnityEngine.Time::get_timeScale"));

    // Хук геттера — единственный способ провести минус через Unity:
    // нативный сеттер обрезает отрицательный timeScale до 0.
    if (get_timeScale)
        DobbyHook((void*) get_timeScale, (void*) hooked_get_timeScale,
                  (void**) &orig_get_timeScale);
}

// Сеттер вызываем только для неотрицательных — он держит внутреннее
// состояние движка согласованным. Отрицательные отдаёт хук геттера.
static void ApplyTimeScale(float v) {
    if (set_timeScale && v >= 0.0f)
        set_timeScale(v);
}

// Фоновый поток: синхронизирует внутреннее состояние, пока включён тумблер
static void TimeScaleKeepAlive() {
    while (!set_timeScale) {
        ResolveTimeScale();
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
                timescale = v; // любое значение: минус, дробь, без ограничений
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

    // TimeScale: резолв icall-ов + хук геттера + фоновый поток
    ResolveTimeScale();
    std::thread(TimeScaleKeepAlive).detach();

    LOGI(OBFUSCATE("Done"));
}

__attribute__((constructor))
void lib_main() {
    std::thread(hack_thread).detach();
}
