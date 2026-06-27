#include "../include/db.hpp"
#include <cstring>

// Prevents C++ name mangling so Python ctypes can locate these exact function signatures
extern "C" {
NexusDB* db_create(const char* path) {
    return new NexusDB(path);
}

void db_destroy(NexusDB* db) {
    delete db;
}

void db_put(NexusDB* db, const char* key, const char* value) {
    db->put(key, value);
}

const char* db_get(NexusDB* db, const char* key) {
    auto val = db->get(key);
    if (val) {
        char* res = new char[val->length() + 1];
        std::strcpy(res, val->c_str());
        return res;
    }
    return nullptr;
}

void db_free_string(const char* str) {
    delete[] str;
}
}