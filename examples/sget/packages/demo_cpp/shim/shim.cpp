// 由 bindgen 自动生成（源: packages/demo_cpp/demo_cpp.bind，许可证 MIT）。头文件按许可直接集成，环境不集成，调用经此代理。
#include <cstdint>

#include "demo_cpp.hpp"

extern "C" __declspec(dllexport)
long long counter_new(const char* name) {
    return (long long)(intptr_t)(new demo::Counter(name));
}

extern "C" __declspec(dllexport)
long long counter_add(long long c, long long v) {
    return ((demo::Counter*)(intptr_t)c)->add((int)v);
}

extern "C" __declspec(dllexport)
long long counter_get(long long c) {
    return ((demo::Counter*)(intptr_t)c)->get();
}

extern "C" __declspec(dllexport)
const char* counter_name(long long c) {
    return ((demo::Counter*)(intptr_t)c)->name();
}

extern "C" __declspec(dllexport)
void counter_reset(long long c) {
    ((demo::Counter*)(intptr_t)c)->reset();
}

extern "C" __declspec(dllexport)
void counter_free(long long c) {
    delete (demo::Counter*)(intptr_t)c;
}
