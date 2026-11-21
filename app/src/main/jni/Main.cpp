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
static std::atomic<bool> reverseTime{false};
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

int scoreMul = 1, coinsMul = 1;

// Do not change or translate the first text unless you know what you are doing
jobjectArray GetFeatureList(JNIEnv *env, jobject context) {
    jobjectArray ret;

    const char *features[] = {
            OBFUSCATE("Category_TimeScale"),
            OBFUSCATE("Toggle_TimeScale enabled"),                     // номер 0
            OBFUSCATE("SeekBar_Speed x0.1-x10_1_100"),                 // номер 1 (значение /10)
            OBFUSCATE("Toggle_Reverse time (negative for slider)"),    // номер 2
            OBFUSCATE("InputText_Custom speed (e.g. -0.75 or 2.5)"),   // номер 3

            OBFUSCATE("Toggle_No death"),                              // номер 4
            OBFUSCATE("Button_Start Invcibility (30 sec duration)"),   // номер 5
            OBFUSCATE("SeekBar_Score multiplier_1_100"),               // номер 6
            OBFUSCATE("SeekBar_Coins multiplier_1_1000"),              // номер 7

            OBFUSCATE("Category_Examples"), //Not counted
            OBFUSCATE("Toggle_The toggle"),
            OBFUSCATE("100_Toggle_True_The toggle 2"),
            OBFUSCATE("110_Toggle_The toggle 3"),
            OBFUSCATE("SeekBar_The slider_1_100"),
            OBFUSCATE("SeekBar_Kittymemory slider example_1_5"),
            OBFUSCATE("Spinner_The spinner_Items 1,Items 2,Items 3"),
            OBFUSCATE("Button_The button"),
            OBFUSCATE("ButtonLink_The button with link_https://www.youtube.com/"),
            OBFUSCATE("ButtonOnOff_The On/Off button"),
            OBFUSCATE("CheckBox_The Check Box"),
            OBFUSCATE("InputValue_Input number"),
            OBFUSCATE("InputValue_1000_Input number 2"),
            OBFUSCATE("1111_InputLValue_Input long number"),
            OBFUSCATE("InputLValue_1000000000000_Input long number 2"),
            OBFUSCATE("InputText_Input text"),
            OBFUSCATE("RadioButton_Radio buttons_OFF,Mod 1,Mod 2,Mod 3"),

            OBFUSCATE("Collapse_Collapse 1"),
            OBFUSCATE("CollapseAdd_Toggle_The toggle"),
            OBFUSCATE("CollapseAdd_Toggle_The toggle"),
            OBFUSCATE("123_CollapseAdd_Toggle_The toggle"),
            OBFUSCATE("122_CollapseAdd_CheckBox_Check box"),
            OBFUSCATE("CollapseAdd_Button_The button"),

            OBFUSCATE("Collapse_Collapse 2_True"),
            OBFUSCATE("CollapseAdd_SeekBar_The slider_1_100"),
            OBFUSCATE("CollapseAdd_InputValue_Input number"),

            OBFUSCATE("RichTextView_This is text view, not fully HTML."
                      "<b>Bold</b> <i>italic</b> <u>underline</u>"
                      "<br />New line <font color='red'>Support colors</font>"
                      "<br/><big>bigger Text</big>"),
            OBFUSCATE("RichWebView_<html><head><style>body{color: white;}</style></head><body>"
                      "This is WebView, with REAL HTML support!"
                      "<div style=\"background-color: darkblue; text-align: center;\">Support CSS</div>"
                      "</body></html>")
    };

    int Total_Feature = (sizeof features / sizeof features[0]);
    ret = (jobjectArray)
            env->NewObjectArray(Total_Feature, env->FindClass(OBFUSCATE("java/lang/String")),
                                env->NewStringUTF(""));

    for (int i = 0; i < Total_Feature; i++)
        env->SetObjectArrayElement(ret, i, env->NewStringUTF(features[i]));

    return (ret);
}

bool btnPressed = false;

#define targetLibName OBFUSCATE("libil2cpp.so")

void Changes(JNIEnv *env, jclass clazz, jobject obj, jint featNum, jstring featName,
             jint value, jlong Lvalue, jboolean boolean, jstring text) {

    switch (featNum) {
        // ===== TimeScale =====
        case 0: // Toggle_TimeScale enabled
            timescaleOn = boolean;
            if (!timescaleOn && set_timeScale)
                set_timeScale(1.0f);   // аналог off(): возврат к нормальной скорости
            break;

        case 1: // SeekBar: 1..100 -> 0.1..10.0
            timescale = (value / 10.0f) * (reverseTime ? -1.0f : 1.0f);
            break;

        case 2: // Toggle_Reverse time (для слайдера)
            reverseTime = boolean;
            timescale = (value / 10.0f) * (reverseTime ? -1.0f : 1.0f);
            break;

        case 3: { // InputText: ручной ввод, минус и дробь — прямо из строки
            if (!text) break;
            const char *str = env->GetStringUTFChars(text, nullptr);
            if (str) {
                float v = strtof(str, nullptr);   // парсит и "-0.75", и "2.5"
                env->ReleaseStringUTFChars(text, str);

                if (v >= -100.0f && v <= 100.0f)  // защита от случайного 99999
                    timescale = v;
                // невалидный/пустой ввод просто игнорируется — скорость не меняется
            }
            break;
        }

        // ===== Ваши фичи из шаблона =====
        case 4:
            // offset, hex
            PATCH_SWITCH(targetLibName, "0x1079728", "C0 03 5F D6", boolean);
            break;

        case 5:
            btnPressed = true;
            break;

        case 6:
            scoreMul = value;
            break;

        case 7:
            coinsMul = value;
            break;

        default:
            break;
    }
}

// ===================== Хуки из шаблона =====================

//CharacterPlayer
void (*StartInvcibility)(void *instance, float duration);

void (*old_Update)(void *instance);
void Update(void *instance) {
    if (instance != nullptr) {
        if (btnPressed) {
            StartInvcibility(instance, 30);
            btnPressed = false;
        }
    }
    return old_Update(instance);
}

void (*old_AddCoins)(void *instance, int count);
void AddCoins(void *instance, int count) {
    return old_AddCoins(instance, count * coinsMul);
}

install_hook_name(AddScore, void *, void *instance, int score) {
    return orig_AddScore(instance, score + scoreMul);
}

// ===================== Потоки =====================

void hack_thread() {
    // libil2cpp.so загружается не сразу — ждём
    while (!isLibraryLoaded(targetLibName)) {
        sleep(1);
    }

    // TimeScale: резолв icall-ов + фоновый поток поддержки
    ResolveTimeScale();
    std::thread(TimeScaleKeepAlive).detach();

#if defined(__aarch64__)
    StartInvcibility = (void (*)(void *, float)) getAbsoluteAddress(targetLibName, OBFUSCATE("0x107A3BC"));

    HOOK(targetLibName, "0x107A2FC", AddCoins, old_AddCoins);
    install_hook_AddScore(getAbsoluteAddress(targetLibName, OBFUSCATE("0x107A2E0")));
    HOOK(targetLibName, "0x1078C44", Update, old_Update);

    INST(targetLibName, "0x23558C", "AnyNameForDetect", true);

#elif defined(__arm__)
    //Put your code here if you want the code to be compiled for armv7 only
#endif

    LOGI(OBFUSCATE("Done"));
}

__attribute__((constructor))
void lib_main() {
    std::thread(hack_thread).detach();
}
