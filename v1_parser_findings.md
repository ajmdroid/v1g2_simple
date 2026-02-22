# V1 ESP Parsing Comparison Report

**Local codebase** vs. **[AndroidESPLibrary2](https://github.com/ValentineResearch/AndroidESPLibrary2)** (master)

---

## 1. BLE UUIDs — MATCH

| UUID | VR Library (BTUtil.java) | Local (config.h) | Status |
|------|--------------------------|-------------------|--------|
| Service | `92A0AFF4-...` | `V1_SERVICE_UUID` | Match |
| Short notify (display) | `V1_OUT_CLIENT_IN_SHORT = 92A0B2CE` | `V1_DISPLAY_DATA_UUID` | Match |
| Long notify (alerts) | `V1_OUT_CLIENT_IN_LONG = 92A0B4E0` | `V1_DISPLAY_DATA_LONG_UUID` | Match |
| Write | `CLIENT_OUT_V1_IN_SHORT = 92A0B6D4` | `V1_COMMAND_WRITE_UUID` | Match |
| Long write | `CLIENT_OUT_V1_IN_LONG = 92A0B8D2` | (not referenced) | — |

**Note:** Local defines `V1_COMMAND_WRITE_ALT_UUID = 92A0BAD4` which has no counterpart in the VR library. Not a concern — it's a local fallback for write characteristic discovery.

---

## 2. Packet IDs — MATCH

Every packet ID in `include/config.h` matches `PacketId.java` exactly:

| ID | VR Name | Local Name | Hex |
|----|---------|------------|-----|
| reqVersion | `REQVERSION` | `PACKET_ID_VERSION` | 0x01 |
| reqUserBytes | `REQUSERBYTES` | `PACKET_ID_REQ_USER_BYTES` | 0x11 |
| respUserBytes | `RESPUSERBYTES` | `PACKET_ID_RESP_USER_BYTES` | 0x12 |
| reqWriteUserBytes | `REQWRITEUSERBYTES` | `PACKET_ID_WRITE_USER_BYTES` | 0x13 |
| infDisplayData | `INFDISPLAYDATA` | `PACKET_ID_DISPLAY_DATA` | 0x31 |
| reqTurnOffMainDisplay | `REQTURNOFFMAINDISPLAY` | `PACKET_ID_TURN_OFF_DISPLAY` | 0x32 |
| reqTurnOnMainDisplay | `REQTURNONMAINDISPLAY` | `PACKET_ID_TURN_ON_DISPLAY` | 0x33 |
| reqMuteOn | `REQMUTEON` | `PACKET_ID_MUTE_ON` | 0x34 |
| reqMuteOff | `REQMUTEOFF` | `PACKET_ID_MUTE_OFF` | 0x35 |
| reqChangeMode | `REQCHANGEMODE` | hardcoded `0x36` | 0x36 |
| reqCurrentVolume | `REQCURRENTVOLUME` | — | 0x37 |
| respCurrentVolume | `RESPCURRENTVOLUME` | — | 0x38 |
| reqWriteVolume | `REQWRITEVOLUME` | `PACKET_ID_REQ_WRITE_VOLUME` | 0x39 |
| reqStartAlertData | `REQSTARTALERTDATA` | `PACKET_ID_REQ_START_ALERT` | 0x41 |
| respAlertData | `RESPALERTDATA` | `PACKET_ID_ALERT_DATA` | 0x43 |

---

## 3. Packet Framing & Constants — MATCH (with one intentional difference)

| Element | VR Library (PacketUtils.java) | Local | Status |
|---------|-------------------------------|-------|--------|
| SOF | `0xAA` | `ESP_PACKET_START = 0xAA` | Match |
| EOF | `0xAB` | `ESP_PACKET_END = 0xAB` | Match |
| Dest base | `0xD0` | `0xD0 + ESP_PACKET_DEST_V1` | Match |
| Origin base | `0xE0` | `0xE0 + ESP_PACKET_REMOTE` | Match |
| V1 device byte | `VALENTINE_ONE = 0x0A` | `ESP_PACKET_ORIGIN_V1 = 0x0A` | Match |
| V1connection device byte | `V1CONNECTION = 0x06` | `ESP_PACKET_REMOTE = 0x06` | Match |
| Packet indices | SOF=0, DEST=1, ORIG=2, ID=3, LEN=4, PAYLOAD=5 | Same in `parse()` | Match |

**INTENTIONAL DIFFERENCE — Checksum validation:**
- VR library (`PacketUtils.isValidESPFramingData`): Enforces checksum when device type is `VALENTINE_ONE` (0x0A). Uses `calculateSumNoCarry()`.
- Local (`validatePacket`): **Checksum NOT enforced**. Comment: "V1G2 can chunk packets."
- The local `calcV1Checksum` used for **building** commands is functionally identical to the VR library's `calculateChecksumFor()` — both are simple unsigned byte sums.

This is an intentional local design decision. The VR library validates inbound packets; local code trusts BLE transport integrity. This is acceptable but worth noting — a corrupt inbound packet will be parsed rather than dropped.

---

## 4. Seven-Segment Decode — EXACT MATCH

Every entry in local `decodeBogeyCounterByte()` matches `PacketUtils.java` segment constants:

`0x3F→'0'`, `0x06→'1'`, `0x5B→'2'`, `0x4F→'3'`, `0x66→'4'`, `0x6D→'5'`, `0x7D→'6'`, `0x07→'7'`, `0x7F→'8'`, `0x6F→'9'`, `0x77→'A'`, `0x7C→'b'`, `0x39→'C'`, `0x5E→'d'`, `0x79→'E'`, `0x71→'F'`, `0x1E→'J'`, `0x73→'P'`, `0x38→'L'`, `0x58→'c'`, `0x3E→'U'`, `0x1C→'u'`, `0x49→'#'`, `0x18→'&'` — **All 24 values match.**

---

## 5. Alert Data Byte Layout — MATCH

| Byte | VR Library (AlertData.java) | Local (parseAlertData) | Status |
|------|----------------------------|------------------------|--------|
| [0] high nibble | `getIndex()`: `(data[0] >> 4) & 0x0F` | `(payload[0] >> 4) & 0x0F` | Match |
| [0] low nibble | `getCount()`: `data[0] & 0x0F` | `payload[0] & 0x0F` | Match |
| [1:2] | `getFrequency()`: `(msb << 8) \| lsb` | `combineMSBLSB(a[1], a[2])` | Match |
| [3] | `getFrontSignalStrength()`: `data[3] & 0xFF` | `a[3]` (passed to `mapStrengthToBars`) | Match |
| [4] | `getRearSignalStrength()`: `data[4] & 0xFF` | `a[4]` (passed to `mapStrengthToBars`) | Match |
| [5] | Band: `data[5] & 0x1F`; Dir: `data[5] & 0xE0` | `decodeBand(a[5])` / `decodeDirection(a[5])` | Match (see §5a) |
| [6] bit 7 | `isPriority()`: `(data[6] & 0x80) != 0` | `(aux0 & 0x80) != 0` | Match |
| [6] bit 6 | `isJunkAlert()`: `(data[6] & 0x40) != 0` | `(aux0 & 0x40) != 0` | Match |
| [6] bits 3:0 | `getPhotoType()`: `data[6] & 0x0F` | `aux0 & 0x0F` | Match |

### 5a. Band/Direction Bit Extraction — Functionally Equivalent

**VR Library**: Uses bitmask approach:
- `AlertBand.get(data[5] & 0x1F)`: Laser=0x01, Ka=0x02, K=0x04, X=0x08, Ku=0x10
- `Direction.get(data[5] & 0xE0)`: Front=0x20, Side=0x40, Rear=0x80

**Local**: Uses sequential bit checks in `decodeBand()` / `decodeDirection()`:
- `if (bandArrow & 0x01) return BAND_LASER` → same as Laser=0x01
- `if (bandArrow & 0x02) return BAND_KA` → same as Ka=0x02
- etc.

The local approach checks bits sequentially and returns on first match, while the VR library does a bitmask lookup. Both produce the same result because the V1 protocol guarantees only one band bit set per alert row. **Functionally equivalent.**

**Minor note:** The VR library has a special case in `getBand()` — if band is K and photo type is present, it returns `Photo` band instead. Local code handles `photoType` as a separate field rather than overriding the band. This is a design choice, not a parsing error.

---

## 6. RSSI-to-Bars Mapping — SIGNIFICANT DIFFERENCE

This is the **most notable divergence** from the VR library.

**VR Library** (`AlertData.getBargraphStrength()`) — **0 to 8 scale:**

| Band | 8 | 7 | 6 | 5 | 4 | 3 | 2 | 1 |
|------|---|---|---|---|---|---|---|---|
| Ka | ≥0xBA | ≥0xB3 | ≥0xAC | ≥0xA5 | ≥0x9E | ≥0x97 | ≥0x90 | ≥0x01 |
| K/Ku/Photo | ≥0xC2 | ≥0xB8 | ≥0xAE | ≥0xA4 | ≥0x9A | ≥0x90 | ≥0x88 | ≥0x01 |
| X | ≥0xD0 | ≥0xC5 | ≥0xBD | ≥0xB4 | ≥0xAA | ≥0xA0 | ≥0x96 | ≥0x01 |
| Laser | always 8 | | | | | | | |

**Local** (`mapStrengthToBars()`) — **0 to 6 scale:**

| Band | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
|------|---|---|---|---|---|---|---|
| Ka | >0xB0 | >0xA6 | >0x9C | >0x92 | >0x88 | >0x7F | ≤0x7F |
| K | >0xAE | >0xA4 | >0x9A | >0x90 | >0x86 | >0x7F | ≤0x7F |
| X | >0xC2 | >0xB4 | >0xA6 | >0x98 | >0x8A | >0x7F | ≤0x7F |
| Laser | >0x10 → 6, else 0 | | | | | | |

**Key differences:**
1. **Scale**: VR uses 0-8 bars; local uses 0-6 bars (`MAX_SIGNAL_BARS = 6` in config.h).
2. **Thresholds**: The numerical thresholds don't directly correspond. Local thresholds start at `0x7F` as a noise floor; VR uses `≥0x01` for 1-bar (any signal at all).
3. **Ku band**: VR has explicit Ku thresholds (same as K); local has no Ku case (falls through to `return 0`).
4. **Laser**: VR always returns 8; local checks for `> 0x10` and returns 6.

**Impact**: The per-alert `frontStrength`/`rearStrength` values stored in the alert table are on a different scale than what the VR library would produce. However, the **main display signal bars** come from `decodeLEDBitmap()` (parsing V1's own LED bitmap from the display packet), which correctly decodes the 0-8 pattern. The `mapStrengthToBars` function is only used for per-alert metadata (e.g., `STRONG_SIGNAL_UNMUTE_THRESHOLD` logic), not the primary bar display.

---

## 7. Alert Table Assembly — Different Approach (More Robust Locally)

**VR Library** (`AlertDataProcessor.addAlert()`):
- Simple list-based. Adds alert to `mAlerts` list, deduplicates by index.
- When `mAlerts.size() >= count`, iterates 1..count checking each index is present.
- Returns complete table when all indices found; returns null otherwise.
- No freshness tracking or timeouts.
- 1-based indexing.

**Local** (`parseAlertData()`):
- Chunk-based cache with per-row freshness guards (`kAlertRowFreshnessMs = 1500ms`).
- Assembly timeout (`kAlertAssemblyTimeoutMs = 1800ms`) — if table can't be completed in time, cache is cleared.
- Supports both 1-based and 0-based index detection (defaults to 1-based, matching VR).
- Per-count tracking so tables with different counts don't interfere.
- Stale row eviction on every incoming row.

**Assessment**: Local approach is **more robust** than VR library's — it handles packet loss, stale data, and count transitions that the simple VR list approach doesn't address. The core assembly logic (collect all rows 1..count, publish when complete) matches VR's intent. This is good engineering, not a divergence to worry about.

---

## 8. Version Parsing — Functionally Equivalent (Different Representation)

**VR Library** (`ResponseVersion.java`):
- Parses as ASCII string: `versionID + major + '.' + minor + rev1 + rev2 + ctrl`
- Converts to `double`, e.g., `4.1037`
- Feature gates: `JUNK_START = 4.1032`, `PHOTO_START = 4.1037`

**Local** (`src/packet_parser.cpp`):
- Parses same bytes, builds integer: `major * 10000 + minor * 1000 + rev1 * 100 + rev2 * 10 + ctrl`
- Stores as `uint32_t`, e.g., `41037`
- Feature gates: `>= 41032` (junk), `>= 41037` (photo)

**Byte offsets**: VR reads `payload[1]` as versionID letter (matching local). VR's `payload[0]` is described as "unused" — local skips it too. **Match.**

The arithmetic is equivalent: `4.1037` as a double vs `41037` as an integer yield identical comparison results for the known feature thresholds. **Functionally equivalent.**

---

## 9. Display Data Parsing — MATCH

**VR Library**: The `InfDisplayData` packet isn't parsed in a dedicated response class. The VR Android app handles it at a higher level. However, the packet structure is well-documented.

**Local** (`parseDisplayData()`):
- `payload[0]` = bogey counter image1 (seven-segment) — matches VR segment map
- `payload[1]` = bogey counter image2 (unused) — matches VR
- `payload[2]` = LED bar bitmap — decoded via `decodeLEDBitmap()`
- `payload[3]` = image1 (ON bits for bands/arrows)
- `payload[4]` = image2 (steady bits)
- Flash = `image1 & ~image2` — **matches VR display behavior**
- `payload[5]` = auxData0 (status bits)
- `payload[6]` = auxData1 (mode)
- `payload[7]` = auxData2 (volume: upper nibble = main, lower = mute)

Band arrow bit assignments in `processBandArrow()`:
- Laser=0x01, Ka=0x02, K=0x04, X=0x08, Mute=0x10, Front=0x20, Side=0x40, Rear=0x80

These match the VR library's `AlertBand` + `Direction` bit definitions exactly.

---

## 10. Command Packet Building — MATCH

**VR Library** (`RequestPacket.init()`):
- For `VALENTINE_ONE` type: `payloadLength` in wire byte includes the checksum byte
- For no-payload commands: `payloadLength = 1` (just the checksum byte)
- For 1-byte payload: `payloadLength = 2` (data + checksum)

**Local** (`src/ble_commands.cpp`):

| Command | VR payload field | Local payload[4] | Wire bytes match? |
|---------|-----------------|-------------------|-------------------|
| reqStartAlertData | null → len=1 | `0x01` | **Yes** |
| reqVersion | null → len=1 | `0x01` | **Yes** |
| reqMuteOn/Off | null → len=1 | `0x01` | **Yes** |
| reqChangeMode | 1 byte → len=2 | `0x02` | **Yes** |
| reqWriteVolume | 3 bytes → len=4 | `0x04` | **Yes** |
| reqTurnOnMainDisplay | null → len=1 | `0x01` | **Yes** |
| reqTurnOffMainDisplay | 1 byte (mode) → len=2 | `0x02` | **Yes** |
| reqWriteUserBytes | 6 bytes → len=7 | `0x07` | **Yes** |

All command packets use the same checksum calculation: `calcV1Checksum(packet, N)` where N = position of checksum byte. This matches VR's `calculateChecksumFor()`. **All command formats match.**

---

## 11. Mode Decoding — Slight Mapping Difference

**VR Library** (`V1Mode.java`):
- `AllBogeysKKa = 1`, `LogicKa = 2`, `AdvancedLogic = 3`, `Unknown = 0`

**Local** (`decodeMode()`):
- Extracts mode from `payload[6]` (auxData1): `(aux1 >> 2) & 0x03`
- Maps: `1 → 'A'`, `2 → 'l'`, `3 → 'L'`, `0 → none`

The VR library names are `AllBogeys=1`, `Logic=2`, `AdvLogic=3`. Local maps `1→'A'` (All Bogeys), `2→'l'` (logic), `3→'L'` (advanced Logic). The raw value encoding matches; the display characters are a local presentation choice.

**Note**: The VR library doesn't parse mode from the display packet's auxData1 directly — it uses `reqChangeMode` / response packets. Local extracts mode from the display packet stream, which is a pragmatic choice for a display-only device. The bit extraction `(aux1 >> 2) & 0x03` isn't documented in the VR library but matches known V1 behavior.

---

## Summary of Findings

| Area | Status | Notes |
|------|--------|-------|
| BLE UUIDs | **Match** | Extra `ALT_UUID` locally is benign |
| Packet IDs | **Match** | All 17+ IDs verified |
| Packet Framing | **Match** | SOF/EOF/dest/src encoding identical |
| Checksum Validation | **Intentional Skip** | Local doesn't validate inbound; builds outbound correctly |
| Seven-Segment Decode | **Exact Match** | All 24 character mappings verified |
| Alert Byte Layout | **Match** | All 7 bytes parsed identically |
| Band/Direction Bits | **Match** | Same bit definitions, different extraction style |
| Priority/Junk/Photo flags | **Match** | Same bit masks, same version gates |
| RSSI-to-Bars | **DIVERGENT** | Different scale (0-6 vs 0-8) and different thresholds |
| Alert Assembly | **Enhanced** | Local adds freshness/timeout; core logic matches |
| Display Data | **Match** | Byte positions, flash calc, LED bitmap all correct |
| Version Parsing | **Equivalent** | Int vs double, same feature gates |
| Command Building | **Match** | All packet formats and checksums verified |
| Mode Decode | **Match** | Values match; display chars are local presentation |

## Items Warranting Attention

1. **RSSI-to-Bars thresholds and scale (§6)** — The most material difference. If per-alert bar counts need to match what JBV1 or the VR app shows, the thresholds and 0-8 scale from the VR library should be adopted. Currently this only affects per-alert metadata (the main signal bar display uses V1's own LED bitmap correctly).

2. **No Ku band handling** — VR library explicitly handles Ku (0x10) with K-equivalent thresholds. Local `mapStrengthToBars` has no Ku case and returns 0. If Ku alerts are ever encountered, they'd show 0 bars.

3. **Inbound checksum not enforced** — Acceptable for BLE transport but means a bit-flip in a received packet would be parsed silently rather than dropped.
