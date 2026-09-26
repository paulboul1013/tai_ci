---
name: execution-workflow
description: Vertical-slice execution loop, migration order, parallel work and subagent delegation, and when to escalate versus continue autonomously. Use for large or multi-file changes, planning a slice, or after repeated failures. 觸發：大型變更／垂直切片／移植順序／平行工作／subagent／受阻／重複失敗／自主執行。
---

# Execution Workflow

## Vertical loop

For each slice:

1. Inspect repository evidence and establish the current observable behavior.
2. Trace dependencies and define affected interfaces and ownership.
3. Implement the smallest executable vertical slice.
4. Compile and run focused tests.
5. Compare against Python where behavior is observable.
6. Diagnose and fix failures, then integrate upstream and downstream.
7. Update migration records only after evidence exists.

A loop is complete when the slice builds, its relevant tests and comparisons
pass, cleanup paths are reviewed, and the project remains runnable.

Use repository inspection, call-graph tracing, experiments, builds, and
debugging to resolve ordinary engineering uncertainty autonomously. Escalate
only product-defining, destructive, irreversible, or repository-undecidable
choices. If implementation can continue, a plan alone is not a deliverable.

## Parallel work

Use subagents when tasks are independent and parallelism materially improves
speed or verification quality. The root agent owns global architecture,
subsystem contracts, shared structures, ownership conventions, dependency
direction, integration, and final acceptance.

Good delegated branches include independent Python-subsystem analysis,
implementation behind a fixed interface, differential tests, isolated failure
diagnosis, ownership/sanitizer/concurrency review, and dependency API research.
Give each agent a distinct architectural boundary; use isolated worktrees for
parallel implementation when practical. Use an independent verifier for major
changes rather than relying on the implementation's reasoning path.
