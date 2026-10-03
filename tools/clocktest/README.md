**English** | [简体中文](README.zh-CN.md)

# Text Clock Face (QlockTwo) Word Selection Testbed

The minimalist text clock face (`src/clock.cpp`, QlockTwo-style matrix) rounds time to 5-minute intervals and highlights combinations of English words.

Test coverage:
1. **Critical boundary times**:
   - 11:58 -> IT IS TWELVE O'CLOCK PM (Noon 12:00 PM boundary)
   - 23:58 -> IT IS TWELVE O'CLOCK AM (Midnight 12:00 AM boundary)
   - 12:30 -> IT IS HALF PAST TWELVE PM
   - 00:03 -> IT IS FIVE PAST TWELVE AM (Rounded to 5 minutes)
   - 11:40 -> IT IS TWENTY TO TWELVE PM
   - 23:40 -> IT IS TWENTY TO TWELVE AM
   - 12:45 -> IT IS QUARTER TO ONE PM
   - 00:45 -> IT IS QUARTER TO ONE AM
2. **12-slot 5-minute word combination state machine verification**:
   - Mutual exclusion of O'CLOCK / PAST / TO
   - Precise combination of HALF / QUARTER / TWENTY / FIVE / TEN
3. **24 hours x 60 minutes (1440 points) exhaustive traversal invariant verification**:
   - AM / PM strictly mutually exclusive and correctly mapping to target hour
   - Target hour strictly within 1..12
   - Sentence syntax combination strictly valid

## How to Run

```bash
cd tools/clocktest && ./build.sh
# Enable AddressSanitizer:
SAN=1 ./build.sh
```
