**English** | [简体中文](README.zh-CN.md)

# HTTP JSON and Streaming JSON Array Parser Testbed

`src/http_json.h` encapsulates JSON parsing logic for firmware network API calls. In particular, `fetchJsonStreamArray` is specifically designed for Cardputer ADV (ESP32-S3 without PSRAM, constrained RAM) to deserialize large arrays item-by-item (such as GitHub 365-day heatmaps or USGS earthquake lists), avoiding tens of kilobytes of heap allocations that cause out-of-memory (OOM) crashes during monolithic parsing.

This testbed uses host g++ together with `stubs/` (mocking `Stream`, `HTTPClient`, time advancement, and delays) to perform comprehensive assertion verification across all boundary conditions without relying on physical hardware or live networks.

## Test Coverage

1. **Standard Streaming Array Parsing (`fetchJsonStreamArray`)**:
   - Multi-item object arrays parsed item-by-item, verifying incrementing indices and field extraction
   - Compatibility with pretty-printed JSON containing spaces, tabs, indentation, and newlines
   - Nested objects and child array composite structure parsing
   - String lists, boolean arrays, and null value handling
2. **Empty Array Handling**:
   - Compact empty array `[]`
   - Empty array containing whitespace and newlines `[ \r\n\t ]`
   - Empty array with `arrayKey` prefix navigation `{"items":[]}`
3. **Prefix Navigation and Key Matching (`arrayKey`)**:
   - Array extraction from nested wrapper objects
   - Graceful return of `false` with error `"array not found"` when target key does not exist
4. **Mid-Stream Truncation and Syntax Errors**:
   - Unexpected EOF inside elements (e.g. `[{"id":1},{"id":` EOF), capturing `json error`
   - Immediate EOF after a trailing comma, capturing syntax error
   - Missing closing bracket `]` after elements, correctly capturing stream timeout
5. **Separator Anomalies**:
   - Invalid separators (e.g. semicolons `[{"a":1};{"b":2}]`), correctly capturing `"unexpected separator"`
   - Missing separators (space-only separation between objects), correctly capturing `"unexpected separator"`
6. **Stream Timeout and Unresponsiveness**:
   - Initial character read timeout (empty stream until timeout), correctly returning `"stream timeout"`
   - Timeout from whitespace exhaustion
7. **Volume Control and Early Termination**:
   - `maxItems` truncation (returns `true` once count is reached, discarding trailing stream)
   - Early termination when `onItem` callback returns `false`, gracefully returning `true`
8. **ArduinoJson Filter Mechanism (`Filter`)**:
   - Verifies retaining only specified fields and discarding redundant data via `JsonDocument filter`
9. **Monolithic JSON Parsing (`fetchJsonHttp`)**:
   - `options.stream = true` single-object streaming parsing
   - `options.stream = false` in-memory full string (`getString`) parsing
   - `filter` field filtering
   - Syntax error reporting with `includeJsonDetail` and custom `jsonError` formats
10. **HTTP Client Status and Configuration Passthrough**:
    - HTTP 404 / 500 status code handling and error reporting
    - `http.begin` failure handling
    - Verification of User-Agent, Authorization headers, connection/read timeouts, and HTTP 1.0 settings

## How to Run

```bash
cd tools/jsontest && ./build.sh
# Enable AddressSanitizer and UndefinedBehaviorSanitizer:
SAN=1 ./build.sh
```
