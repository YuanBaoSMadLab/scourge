// 由 bindgen 自动生成（源: D:\Scourge\examples\bindgen\packages/sqlite3/sqlite3.bind，许可证 PublicDomain）。头文件按许可直接集成，环境不集成，调用经此代理。
#include <cstdint>

#include "sqlite3.h"

extern "C" __declspec(dllexport)
long long open_db(const char* path, long long flags) {
    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(path, (sqlite3**)&db, (int)flags, nullptr);
    if (rc != SQLITE_OK) {
    if (db != nullptr) sqlite3_close(db);
    return 0;
    }
    return (long long)(intptr_t)db;
}

extern "C" __declspec(dllexport)
void close_db(long long db) {
    sqlite3_close((sqlite3*)(intptr_t)db);
}

extern "C" __declspec(dllexport)
long long exec_sql(long long db, const char* sql) {
    return (long long)sqlite3_exec((sqlite3*)(intptr_t)db, sql, nullptr, nullptr, nullptr);
}

extern "C" __declspec(dllexport)
long long prepare(long long db, const char* sql) {
    sqlite3* stmt = nullptr;
    int rc = sqlite3_prepare_v2((sqlite3*)(intptr_t)db, sql, -1, (sqlite3_stmt**)&stmt, nullptr);
    if (rc != SQLITE_OK) return 0;
    return (long long)(intptr_t)stmt;
}

extern "C" __declspec(dllexport)
long long bind_int(long long stmt, long long idx, long long v) {
    return (long long)sqlite3_bind_int((sqlite3_stmt*)(intptr_t)stmt, (int)idx, (int)v);
}

extern "C" __declspec(dllexport)
long long bind_text(long long stmt, long long idx, const char* v) {
    return (long long)sqlite3_bind_text((sqlite3_stmt*)(intptr_t)stmt, (int)idx, v, -1, SQLITE_TRANSIENT);
}

extern "C" __declspec(dllexport)
long long bind_double(long long stmt, long long idx, double v) {
    return (long long)sqlite3_bind_double((sqlite3_stmt*)(intptr_t)stmt, (int)idx, v);
}

extern "C" __declspec(dllexport)
long long step(long long stmt) {
    return (long long)sqlite3_step((sqlite3_stmt*)(intptr_t)stmt);
}

extern "C" __declspec(dllexport)
long long column_int(long long stmt, long long idx) {
    return (long long)sqlite3_column_int((sqlite3_stmt*)(intptr_t)stmt, (int)idx);
}

extern "C" __declspec(dllexport)
double column_double(long long stmt, long long idx) {
    return sqlite3_column_double((sqlite3_stmt*)(intptr_t)stmt, (int)idx);
}

extern "C" __declspec(dllexport)
const char* column_text(long long stmt, long long idx) {
    return (const char*)sqlite3_column_text((sqlite3_stmt*)(intptr_t)stmt, (int)idx);
}

extern "C" __declspec(dllexport)
void finalize(long long stmt) {
    sqlite3_finalize((sqlite3_stmt*)(intptr_t)stmt);
}

extern "C" __declspec(dllexport)
long long err_code(long long db) {
    return (long long)sqlite3_extended_errcode((sqlite3*)(intptr_t)db);
}

extern "C" __declspec(dllexport)
const char* err_msg(long long db) {
    return sqlite3_errmsg((sqlite3*)(intptr_t)db);
}
