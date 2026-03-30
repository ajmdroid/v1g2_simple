#!/usr/bin/env python3
import re

path = 'test/test_wifi_display_colors_api_service/test_wifi_display_colors_api_service.cpp'
src = open(path).read()

src = src.replace('        []() { return false; });', '        [](void* /*ctx*/) { return false; }, nullptr);')
src = src.replace('        []() { return true; });', '        [](void* /*ctx*/) { return true; }, nullptr);')
src = re.sub(r'handleApiPreview\(server, makeRuntime\(rt\),\s*nullptr\)', 'handleApiPreview(server, makeRuntime(rt), nullptr, nullptr)', src)
src = re.sub(r'handleApiClear\(server, makeRuntime\(rt\),\s*nullptr\)', 'handleApiClear(server, makeRuntime(rt), nullptr, nullptr)', src)

open(path, 'w').write(src)
print(f'std::function remaining: {src.count("std::function")}')
print(f'old []() {{ remaining: {src.count("[]() {")}')
print('done')
