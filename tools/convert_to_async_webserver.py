#!/usr/bin/env python3
"""
Convert wifi_manager.cpp from WebServer to ESPAsyncWebServer
This script performs automated refactoring for the async web server migration.
"""

import re
import sys

def convert_wifi_manager(input_file, output_file):
    with open(input_file, 'r') as f:
        content = f.read()
    
    # 1. Fix serveLittleFSFileHelper signature
    content = re.sub(
        r'bool serveLittleFSFileHelper\(WebServer& server, const char\* path, const char\* contentType\)',
        'bool serveLittleFSFileHelper(AsyncWebServerRequest *request, const char* path, const char* contentType)',
        content
    )
    
    # 2. Fix serveLittleFSFileHelper implementation
    old_helper = r'''bool serveLittleFSFileHelper\(AsyncWebServerRequest \*request, const char\* path, const char\* contentType\) \{
    File file = LittleFS\.open\(path, "r"\);
    if \(!file\) \{
        return false;
    \}
    server\.streamFile\(file, contentType\);
    file\.close\(\);
    return true;
\}'''
    
    new_helper = '''bool serveLittleFSFileHelper(AsyncWebServerRequest *request, const char* path, const char* contentType) {
    if (!LittleFS.exists(path)) {
        return false;
    }
    request->send(LittleFS, path, contentType);
    return true;
}'''
    
    content = re.sub(old_helper, new_helper, content, flags=re.DOTALL)
    
    # 3. Convert all server.on() lambda signatures - pattern 1: single line
    content = re.sub(
        r'server\.on\(([^,]+),\s*HTTP_(GET|POST|PUT|DELETE|PATCH),\s*\[this\]\(\)\s*\{',
        r'server.on(\1, HTTP_\2, [this](AsyncWebServerRequest *request) {',
        content
    )
    
    # 4. Convert process() method to empty implementation
    old_process = r'''void WiFiManager::process\(\) \{
    server\.handleClient\(\);
    
    if \(staEnabledByConfig\) \{
        checkSTAConnection\(\);
    \}
\}'''
    
    new_process = '''void WiFiManager::process() {
    // AsyncWebServer handles requests automatically - no handleClient() needed
    
    if (staEnabledByConfig) {
        checkSTAConnection();
    }
}'''
    
    content = re.sub(old_process, new_process, content, flags=re.DOTALL)
    
    # 5. Convert server.send() calls
    content = re.sub(r'\bserver\.send\(', 'request->send(', content)
    
    # 6. Convert server.arg() calls
    content = re.sub(r'\bserver\.arg\(', 'request->arg(', content)
    
    # 7. Convert server.hasArg() calls
    content = re.sub(r'\bserver\.hasArg\(', 'request->hasArg(', content)
    
    # 8. Convert server.uri() calls
    content = re.sub(r'\bserver\.uri\(\)', 'request->url()', content)
    
    # 9. Convert server.header() calls
    content = re.sub(r'\bserver\.header\(', 'request->header(', content)
    
    # 10. Fix serveLittleFSFile method signature in class
    content = re.sub(
        r'bool WiFiManager::serveLittleFSFile\(const char\* path, const char\* contentType\)',
        'bool WiFiManager::serveLittleFSFile(AsyncWebServerRequest *request, const char* path, const char* contentType)',
        content
    )
    
    # 11. Fix serveLittleFSFile implementation to pass request
    content = re.sub(
        r'return serveLittleFSFileHelper\(server, path, contentType\);',
        'return serveLittleFSFileHelper(request, path, contentType);',
        content
    )
    
    # 12. Fix all serveLittleFSFile() calls in lambda to pass request
    content = re.sub(
        r'serveLittleFSFile\(([^)]+)\)',
        r'serveLittleFSFile(request, \1)',
        content
    )
    
    # Write output
    with open(output_file, 'w') as f:
        f.write(content)
    
    print(f"✅ Converted {input_file} -> {output_file}")
    print("⚠️  Manual review needed for:")
    print("   - Streaming/chunked response handlers (handleLogsData, handleSerialLog, handleSerialLogContent)")
    print("   - Redirect handlers with sendHeader()")
    print("   - Any complex response patterns")

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print("Usage: python3 convert_to_async_webserver.py input.cpp output.cpp")
        sys.exit(1)
    
    convert_wifi_manager(sys.argv[1], sys.argv[2])
