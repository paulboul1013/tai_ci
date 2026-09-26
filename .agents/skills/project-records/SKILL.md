---
name: project-records
description: Rules for keeping ARCHITECTURE.md, PORTING_PLAN.md (migration states) and ACCEPTANCE.md in sync with evidence. Use when updating status, recording differences, or syncing docs after a change. 觸發：ARCHITECTURE.md／ACCEPTANCE.md／狀態定義／狀態更新／文件同步。
---

# Project Records

## `ARCHITECTURE.md`

Keep `ARCHITECTURE.md` as the repository/subsystem index and route detailed
architecture into `docs/architecture/`. Update the index when directory layout
or subsystem boundaries change; update the owning disclosed reference when a
change affects state, data flow, event flow, rendering, threads, or ownership.

## `PORTING_PLAN.md`

For every subsystem, maintain Python source, C destination, dependencies,
migration state, behavioral evidence, and known discrepancies. Use these states:

`NOT_ANALYZED`, `ANALYZED`, `INTERFACE_DEFINED`, `IMPLEMENTING`, `VALIDATING`,
`COMPLETE`, `BLOCKED`.

Advance state only when repository evidence satisfies the new state. Record
intentional differences where they can be audited later.

## `ACCEPTANCE.md`

Keep objective whole-project completion conditions and conservative evidence.
Check an item only when every clause is demonstrated. A successful build or one
vertical slice does not satisfy browser-level acceptance.

`ACCEPTANCE.md` itself stays short: title, status line, the checklist, the
rules paragraphs, and a "Slice evidence" table (newest first). New slice
evidence = a new dated file under `docs/acceptance/<YYYY-MM-DD>-<slug>.md`
plus one row in that table; never append long logs to `ACCEPTANCE.md`
directly — see `docs/acceptance/README.md`.

Documentation sync is complete when every changed architectural fact, migration
state, acceptance claim, and known discrepancy has exactly one authoritative
record and agrees with the implementation and tests.
