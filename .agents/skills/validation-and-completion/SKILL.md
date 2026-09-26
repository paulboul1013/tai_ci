---
name: validation-and-completion
description: Evidence ladder and completion rules: unit/integration tests, Python-oracle comparison, sanitizers and leak checks, screenshot/pixel comparison, independent review, and when a subsystem may be marked COMPLETE. Use before claiming a task, slice, or acceptance item is done. 觸發：測試策略／驗證證據／sanitizer／記憶體洩漏／驗收宣告／完成宣告／回歸／截圖比較／程式碼審查。
---

# Validation and Completion

## Evidence ladder

Validate the affected scope with all applicable layers:

1. focused unit or component test;
2. integration test across changed boundaries;
3. Python-oracle comparison for observable behavior;
4. representative native browser workflow;
5. sanitizer run when supported by the environment;
6. independent adversarial review for important or large changes.

Rendering comparisons should prefer structure and stable key regions unless
fonts, backend, and environment are controlled well enough for pixel-perfect
assertions. Record the comparison boundary and known omissions.

For agent-driven SDL window input and screenshot evidence, follow
the `native-window-verification` skill ([SKILL.md](../native-window-verification/SKILL.md)). Its real-window
pixel check complements the focused `presentation_dummy` event test.

Memory safety is a first-class acceptance condition. Reproducible corruption,
use-after-free, or systematic leaks block completion. Distinguish a passing
ASan/UBSan run from leak validation when LeakSanitizer cannot execute.

## Subsystem completion

Mark a subsystem `COMPLETE` only when all are true:

1. required functionality exists;
2. it builds cleanly;
3. relevant tests pass;
4. important behavior matches the Python reference;
5. resource ownership has been reviewed;
6. intentional differences are documented;
7. upstream and downstream integration is tested.

The migration is complete only when the native browser executes representative
browser workflows without a Python runtime. Compilation, scaffolding, a single
PNG, or isolated component tests are intermediate evidence, not completion.

An independent verifier should actively seek behavioral gaps, edge cases,
ownership errors, API contract violations, concurrency hazards, regressions,
and missing tests, and may reject the implementation.
