#pragma once
#ifdef __ANDROID__
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "gmstream", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "gmstream", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "gmstream", __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) (fprintf(stderr, "I: " __VA_ARGS__), fputc('\n', stderr))
#define LOGW(...) (fprintf(stderr, "W: " __VA_ARGS__), fputc('\n', stderr))
#define LOGE(...) (fprintf(stderr, "E: " __VA_ARGS__), fputc('\n', stderr))
#endif
