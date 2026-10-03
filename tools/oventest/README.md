**English** | [简体中文](README.zh-CN.md)

# Hash Oven Brute-Force Guess Generation Testbed

The Futility Cracker mode in `src/hash_oven.cpp`: generates brute-force candidate passwords based on the currently selected keyspace tier and trial index (0..total-1).

Test coverage:
1. **Keyspace preset character set and length mathematical verification**:
   - Character sets contain no duplicate characters
   - Total keyspace size matches `total == base^len` (e.g. 10^4, 26^4, 26^6, 26^8, 62^8)
   - Preset solution characters for each tier exist in the corresponding character set
2. **Full keyspace index coverage and injective collision-free verification**:
   - 4-Digits tier: Exhaustive traversal of all 10,000 indices (0..9999), verifying strict 1-to-1 mapping, no duplicates, no omissions
   - 4-Lower tier: Exhaustive traversal of all 456,976 indices (0..456,975), verifying complete coverage
   - Large keyspace tiers: 100,000-point stepped sampling across space, verifying 100% identity in bidirectional encode/decode roundtrips
3. **Preset solution hit generation**:
   - Verify that preset solutions for each tier are accurately generated at their respective indices, with index strictly less than `total`
4. **Buffer truncation and invalid parameter protection**:
   - Safe truncation for small buffers without out-of-bounds writes
   - Safeguards against invalid keyspace indices and null buffers

## How to Run

```bash
cd tools/oventest && ./build.sh
# Enable AddressSanitizer:
SAN=1 ./build.sh
```
