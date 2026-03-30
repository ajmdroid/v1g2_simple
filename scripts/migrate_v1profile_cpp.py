#!/usr/bin/env python3
"""Migrate wifi_v1_profile_api_service.cpp from std::function to fn-ptr+ctx."""
import re

path = 'src/modules/wifi/wifi_v1_profile_api_service.cpp'
src = open(path).read()

# Handler signatures + checkRateLimit calls
for fn_name in ['handleApiProfileSave', 'handleApiProfileDelete',
                'handleApiSettingsPull', 'handleApiSettingsPush']:
    src = src.replace(
        f'void {fn_name}(WebServer& server,\n                          const Runtime& runtime,\n                          const std::function<bool()>& checkRateLimit) {{\n    if (checkRateLimit && !checkRateLimit()) return;',
        f'void {fn_name}(WebServer& server,\n                          const Runtime& runtime,\n                          bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {{\n    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;'
    )

# Note: handleApiSettingsPull and handleApiSettingsPush have different indent
src = src.replace(
    'void handleApiSettingsPull(WebServer& server,\n                          ',
    'void handleApiSettingsPull(WebServer& server,\n                           '
)
src = src.replace(
    'void handleApiSettingsPush(WebServer& server,\n                          ',
    'void handleApiSettingsPush(WebServer& server,\n                           '
)

# Fix: the indentation in cpp matches the actual function defs — let's just do a broad replace
old_sigs = [
    ('void handleApiProfileSave(WebServer& server,\n                          const Runtime& runtime,\n                          const std::function<bool()>& checkRateLimit)',
     'void handleApiProfileSave(WebServer& server,\n                          const Runtime& runtime,\n                          bool (*checkRateLimit)(void* ctx), void* rateLimitCtx)'),
    ('void handleApiProfileDelete(WebServer& server,\n                            const Runtime& runtime,\n                            const std::function<bool()>& checkRateLimit)',
     'void handleApiProfileDelete(WebServer& server,\n                            const Runtime& runtime,\n                            bool (*checkRateLimit)(void* ctx), void* rateLimitCtx)'),
    ('void handleApiSettingsPull(WebServer& server,\n                           const Runtime& runtime,\n                           const std::function<bool()>& checkRateLimit)',
     'void handleApiSettingsPull(WebServer& server,\n                           const Runtime& runtime,\n                           bool (*checkRateLimit)(void* ctx), void* rateLimitCtx)'),
    ('void handleApiSettingsPush(WebServer& server,\n                           const Runtime& runtime,\n                           const std::function<bool()>& checkRateLimit)',
     'void handleApiSettingsPush(WebServer& server,\n                           const Runtime& runtime,\n                           bool (*checkRateLimit)(void* ctx), void* rateLimitCtx)'),
]

for old, new in old_sigs:
    if old in src:
        src = src.replace(old, new)
        print(f'  Replaced: {old[:60]}...')
    else:
        print(f'  NOT FOUND: {old[:60]}...')

# checkRateLimit calls
src = src.replace('if (checkRateLimit && !checkRateLimit()) return;',
                  'if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;')

# Runtime field calls
replacements = [
    ('runtime.listProfileNames()',       'runtime.listProfileNames(runtime.listProfileNamesCtx)'),
    ('runtime.loadProfileSummary(name, profile)',
                                         'runtime.loadProfileSummary(name, profile, runtime.loadProfileSummaryCtx)'),
    ('runtime.loadProfileJson(name, profileJson)',
                                         'runtime.loadProfileJson(name, profileJson, runtime.loadProfileJsonCtx)'),
    ('runtime.parseSettingsJson(settingsObj, settingsBytes)',
                                         'runtime.parseSettingsJson(settingsObj, settingsBytes, runtime.parseSettingsJsonCtx)'),
    ('runtime.parseSettingsJson(rootObj, settingsBytes)',
                                         'runtime.parseSettingsJson(rootObj, settingsBytes, runtime.parseSettingsJsonCtx)'),
    ('runtime.parseSettingsJson(settingsObj, bytes)',
                                         'runtime.parseSettingsJson(settingsObj, bytes, runtime.parseSettingsJsonCtx)'),
    ('runtime.parseSettingsJson(rootObj, bytes)',
                                         'runtime.parseSettingsJson(rootObj, bytes, runtime.parseSettingsJsonCtx)'),
    ('runtime.saveProfile(name, description, displayOn, settingsBytes, saveError)',
                                         'runtime.saveProfile(name, description, displayOn, settingsBytes, saveError, runtime.saveProfileCtx)'),
    ('runtime.backupToSd()',              'runtime.backupToSd(runtime.backupToSdCtx)'),
    ('runtime.deleteProfile(name)',       'runtime.deleteProfile(name, runtime.deleteProfileCtx)'),
    ('runtime.v1Connected()',             'runtime.v1Connected(runtime.v1ConnectedCtx)'),
    ('runtime.hasCurrentSettings()',      'runtime.hasCurrentSettings(runtime.hasCurrentSettingsCtx)'),
    ('runtime.currentSettingsJson()',     'runtime.currentSettingsJson(runtime.currentSettingsJsonCtx)'),
    ('runtime.requestUserBytes()',        'runtime.requestUserBytes(runtime.requestUserBytesCtx)'),
    ('runtime.writeUserBytes(bytes)',     'runtime.writeUserBytes(bytes, runtime.writeUserBytesCtx)'),
    ('runtime.setDisplayOn(displayOn)',   'runtime.setDisplayOn(displayOn, runtime.setDisplayOnCtx)'),
    ('runtime.loadProfileSettings(profileName, bytes, displayOn)',
                                         'runtime.loadProfileSettings(profileName, bytes, displayOn, runtime.loadProfileSettingsCtx)'),
]

for old, new in replacements:
    count = src.count(old)
    if count > 0:
        src = src.replace(old, new)
        print(f'  [{count}x] {old[:60]}')
    else:
        print(f'  [0x] NOT FOUND: {old[:60]}')

open(path, 'w').write(src)
print(f'\nstd::function remaining: {src.count("std::function")}')
print('done')
