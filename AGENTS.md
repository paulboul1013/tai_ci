# Browser C Port — Agent Router

## Always-on contract

Rebuild the repository's Python browser as an engineering-quality native C17
browser. Preserve observable behavior and conceptual architecture; translate
ownership, lifetime, and subsystem boundaries rather than Python syntax.

Use this authority order whenever sources disagree:

1. observable Python browser behavior
2. Python tests
3. repository documentation
4. this router and its disclosed references
5. engineering judgment

Resolve uncertainty by inspecting or running the executable oracle. Record
intentional differences; never invent browser behavior silently. Continue
through implementation and verification while the repository provides enough
evidence. Ask only for product-defining, destructive, irreversible, or
repository-undecidable choices.

## Progressive disclosure

Match the current request, files being changed, and concepts encountered
against the trigger words below. Before acting, read **only every matching
reference in full**. Do not preload unmatched references. Re-run this routing
step whenever the task expands into another branch.

- **Oracle / Python reference / behavioral equivalence / differential / porting strategy / intentional difference** → read [`docs/agents/oracle-and-porting.md`](docs/agents/oracle-and-porting.md).
- **C17 toolchain / CMake / Ninja / dependency choice / library replacement / SDL / Cairo / HarfBuzz / FriBidi / FreeType / QuickJS / curl / OpenSSL / zlib / image loading / image codec / PNG / WebP / stb_image / libwebp / utf8proc / profiling** → read [`docs/agents/native-stack.md`](docs/agents/native-stack.md).
- **Architecture boundary / subsystem interface / ownership transfer / object lifetime / allocation cleanup / memory safety / use-after-free / memory corruption / opaque struct / dependency direction** → read [`docs/agents/architecture-and-ownership.md`](docs/agents/architecture-and-ownership.md).
- **Test strategy / verification evidence / sanitizer / memory leak / acceptance claim / completion claim / regression / screenshot / screenshot comparison / pixel comparison / code review** → read [`docs/agents/validation-and-completion.md`](docs/agents/validation-and-completion.md).
- **Large change / vertical slice / migration order / parallel work / subagent / blocked / repeated failure / autonomous execution** → read [`docs/agents/execution-workflow.md`](docs/agents/execution-workflow.md).
- **Repository structure / directory layout / source layout / subsystem map / 目錄結構 / 檔案架構** → read [`ARCHITECTURE.md`](ARCHITECTURE.md).
- **ARCHITECTURE.md / PORTING_PLAN.md / ACCEPTANCE.md / migration state / status update / documentation sync** → read [`docs/agents/project-records.md`](docs/agents/project-records.md).

Completion means the requested behavior is implemented, built, tested, and
integrated at the affected boundaries—not merely planned or scaffolded.
