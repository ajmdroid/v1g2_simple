#pragma once

#include "FS.h"

class LittleFSClass : public fs::FS {
public:
    LittleFSClass()
        : fs::FS(std::filesystem::temp_directory_path() / "codex_littlefs_mock") {}

    bool begin(bool = false, const char* = "/littlefs", uint8_t = 10, const char* = "storage") {
        return true;
    }

    void end() {}
};

inline LittleFSClass LittleFS;
