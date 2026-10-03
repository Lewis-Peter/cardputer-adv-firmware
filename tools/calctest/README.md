**English** | [简体中文](README.zh-CN.md)

# calctest/ — Calculator & Unit Conversion Testbed

Directly compiles `src/calc.cpp` (recursive-descent expression parser) and `src/conv.cpp` (7 unit conversion categories) to run on the host system. All Arduino / M5Unified / Preferences dependencies are stubbed out in `stubs/` with empty implementations.

## Test Coverage (107 Items)

### calc.cpp — Expression Evaluation

| Group | Coverage |
|---|---|
| 1. Basic Arithmetic | `+`, `-`, `*`, `/`, `%`, zero, positive and negative integers |
| 2. Precedence & Associativity | `*` precedes `+`, left-associative subtraction, parentheses, right-associative exponentiation, `-2^2 = -4` |
| 3. Unary Operators | `-x`, `+x`, double / triple negation |
| 4. Decimals | `.5` without leading zero, `1.5+1.5`, integer results without decimal point, `1/3` precision |
| 5. Error Handling | `1/0`, `0/0`, `5%0`, `((1)`, `1)`, `1..2`, empty expressions, overly long inputs |
| 6. Factorials | `0!`, `5!`, `10!`, `170!` (upper limit), errors on `171!`, `1.5!` |
| 7. Exponentiation | `2^10`, `2^0`, `0^0`, `(-2)^3` |
| 8. Formatting | Integers without decimal points, `-0`, scientific notation, NaN error handling |
| 9. Consecutive Operators | `2++3`, `2+-3`, `2*+3` (valid unary operators), error on `2**3` |
| 10. Deep Recursion Protection | 24 levels of nested parentheses without crashing |
| 11. calcKey State Machine | Digits/operators after evaluation, backspace, Ans, Clear |

### conv.cpp — Unit Conversion

| Category | Authoritative Reference Values & Roundtrips |
|---|---|
| Length | 1 inch = 2.54 cm (exact), 1000 m ↔ 1 km, 1 mi ≈ 1609.34 m, 1 nmi = 1852 m |
| Mass | 1 kg = 1000 g, 1 oz = 28.3495 g, 1 lb = 453.592 g |
| Temperature | **-40°C = -40°F** (unique intersection), 0°C = 32°F, 100°C = 212°F, 0°C = 273.15 K |
| Volume | 1 L = 1000 mL, 1 gal = 3785.41 mL |
| Speed | 1 m/s = 3.6 km/h (exact), 36 km/h = 10 m/s |
| Time | 1 h = 3600 s, 1 wk = 7 day |
| Area | 1 m2 = 10000 cm2, 1 ha = 10000 m2, 1 acre = 4046.86 m2 |
| convKey State Machine | Decimal point, minus sign, backspace, S swap, category switching, convEnter deduplication |

## Discovered Issues / Known Behaviors

**No bugs found; all are known design behaviors:**

- **`-0` Display Behavior**: `fmtNum(-0.0)` outputs `"-0"` under glibc (standard behavior of `snprintf("%.0f", -0.0)`). On hardware, this is rarely triggered (e.g. `0*-1`), and the impact is acceptable; completely eliminating it requires a special check for `-0.0` (see `src/calc.cpp`, line 108). The testbed includes relaxed assertions accepting either `"0"` or `"-0"`.
- **`%.6g` Precision Truncation**: `recalc()` uses `%.6g` (6 significant digits) at line 62 of `conv.cpp`. This causes 1609.344 m to display as "1609.34", dropping the last decimal place of the theoretical value. This is an intentional display precision tradeoff, not an inaccurate conversion factor.
- **Key Driver Coupled with Evaluation**: `calcKey`/`convKey` are hybrid key-event state machines and cannot call `parseExpr` directly without traversing the full key path. The testbed feeds input character-by-character via `calcKey` to replicate actual keypad pathways. For finer-grained unit testing in the future, exposing `fmtNum` and `recalc` as testable helpers (guarded with `#ifdef HOST_TEST`) is recommended.

## How to Run

```bash
cd tools/calctest && ./build.sh
# Enable AddressSanitizer + UBSan:
SAN=1 ./build.sh
```
