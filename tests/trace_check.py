#!/usr/bin/env python3
"""Structural validator for Chrome/Perfetto JSON traces (native MeasureTime).

    python3 tests/trace_check.py TRACE...              # validate files
    python3 tests/trace_check.py --format TEST_TRACE   # CTest trace_format

`--format` makes test_trace write a 5-thread stress trace, validates it, then
feeds the validator corrupted variants that it must reject.
"""

import json
import pathlib
import re
import subprocess
import sys
import tempfile


class TraceError(AssertionError):
    pass


def require(condition, message):
    if not condition:
        raise TraceError(message)


def load(path):
    """Return the event list; json.load fails on an unterminated trace."""
    with open(path, encoding="utf-8") as handle:
        document = json.load(handle)
    require(isinstance(document, dict) and isinstance(document.get("traceEvents"), list),
            "top level must be {\"traceEvents\": [...]}")
    return document["traceEvents"]


def check(events, async_ok=True):
    """Validate events; return {tid: thread name}."""
    require(events, "trace has no events")
    first = events[0]
    require(first.get("name") == "process_name" and first.get("ph") == "M"
            and first.get("args") == {"name": "Browser"},
            "first event must be process_name Browser")
    names = {}
    stacks = {}
    last_ts = {}
    open_async = {}
    for index, event in enumerate(events[1:], 1):
        where = "event {} {}".format(index, event)
        require(event.get("pid") == 1, where + ": pid must be 1")
        require(isinstance(event.get("ts"), int), where + ": ts must be an int")
        tid = event.get("tid")
        require(isinstance(tid, int), where + ": tid must be an int")
        ph = event.get("ph")
        if ph == "M":
            require(event.get("name") == "thread_name"
                    and event.get("cat") == "__metadata", where + ": bad metadata")
            require(tid not in names, where + ": thread named twice")
            require(isinstance(event.get("args", {}).get("name"), str),
                    where + ": thread name must be a string")
            names[tid] = event["args"]["name"]
            continue
        require(tid in names, where + ": event before its thread_name")
        require(event.get("cat") == "_", where + ": cat must be _")
        require(isinstance(event.get("name"), str), where + ": name must be a string")
        require(event["ts"] >= last_ts.get(tid, 0), where + ": ts went backwards")
        last_ts[tid] = event["ts"]
        stack = stacks.setdefault(tid, [])
        if ph == "B":
            stack.append(event["name"])
        elif ph == "E":
            require(stack and stack[-1] == event["name"],
                    where + ": E does not close the innermost B {}".format(stack[-1:]))
            stack.pop()
        elif ph in ("b", "e"):
            require(async_ok, where + ": async events not allowed here")
            ident = event.get("id")
            require(isinstance(ident, int) and ident > 0, where + ": bad async id")
            if ph == "b":
                require(ident not in open_async, where + ": async id reused")
                open_async[ident] = event["name"]
            else:
                require(open_async.get(ident) == event["name"],
                        where + ": e without a matching b")
                del open_async[ident]
        elif ph == "i":
            require(event.get("s") == "t", where + ": instant must be thread scoped")
        else:
            raise TraceError(where + ": unknown phase")
    for tid, stack in stacks.items():
        require(not stack, "thread {} left spans open: {}".format(tid, stack))
    require(not open_async, "async slices left open: {}".format(open_async))
    return names


def vocabulary(test_trace):
    """Return (span names, fetch name regexes, instant names) from
    `test_trace --vocabulary`."""
    output = subprocess.run([str(test_trace), "--vocabulary"], check=True,
                            capture_output=True, text=True).stdout
    spans, fetches, instants = set(), [], set()
    for line in output.splitlines():
        phase, name = line.split("\t", 1)
        if phase == "B":
            spans.add(name)
        elif phase == "i":
            instants.add(name)
        else:
            fetches.append(re.compile(re.escape(name).replace("%zu", r"\d+") + "$"))
    return spans, fetches, instants


def name_allowed(event, spans, fetches, instants):
    name = event["name"]
    if event["ph"] in "BE":
        return name in spans
    if event["ph"] == "i":
        return name in instants
    return any(pattern.match(name) for pattern in fetches)


def rejects(events, label):
    try:
        check(events)
    except TraceError:
        return
    raise AssertionError("validator accepted a corrupted trace: " + label)


def format_test(test_trace):
    with tempfile.TemporaryDirectory(prefix="tai-trace-format-") as directory:
        path = pathlib.Path(directory) / "stress.json"
        subprocess.run([str(test_trace), "--stress", str(path)], check=True)
        events = load(path)
        names = check(events)
        require(len(names) == 5, "expected 5 named threads, got {}".format(names))
        require(sorted(names.values()).count("Browser thread") == 1,
                "main thread must be named Browser thread")
        vocab = vocabulary(test_trace)
        for event in events[1:]:
            if event["ph"] != "M":
                require(name_allowed(event, *vocab),
                        "name outside the vocabulary: " + event["name"])
        durations = sum(1 for event in events if event["ph"] in "BE")
        # 4 from the args events (Tab.load and commit) before the workers.
        require(durations == 5 * 10000 * 4 + 4, "lost events: {}".format(durations))
        hostile = 'http://h/"q"\\b\n\t\x01end'
        tab_load = next(e for e in events if e["name"] == "Task: Tab.load")
        require(tab_load["args"] == {"url": hostile, "tab": 3,
                                 "generation": 2 ** 64 - 1},
                "Tab.load args: {}".format(tab_load.get("args")))
        document = next(e for e in events if e["ph"] == "b"
                        and e["name"] == "Network I/O: document")
        require(document["args"] == {"url": hostile},
                "fetch args: {}".format(document.get("args")))
        dropped = next(e for e in events if e["ph"] == "i")
        require(dropped["args"] == {"current_generation": None, "reason": None},
                "instant args: {}".format(dropped.get("args")))

        b_index = next(i for i, e in enumerate(events) if e.get("ph") == "B")
        rejects(events[:b_index + 1], "unclosed B")
        swapped = [dict(e) for e in events]
        swapped[b_index]["name"] = "render"
        rejects(swapped, "mismatched B/E names")
        a_index = next(i for i, e in enumerate(events) if e.get("ph") == "e")
        rejects(events[:a_index] + events[a_index + 1:], "unclosed async slice")
        meta_index = next(i for i, e in enumerate(events) if e.get("ph") == "M"
                          and e.get("name") == "thread_name")
        rejects(events[:meta_index] + events[meta_index + 1:], "unnamed thread")
        text = path.read_text(encoding="utf-8")
        require(text.endswith("]}"), "trace must end with ]}")
        truncated = pathlib.Path(directory) / "truncated.json"
        truncated.write_text(text[:-2], encoding="utf-8")
        try:
            load(truncated)
        except json.JSONDecodeError:
            pass
        else:
            raise AssertionError("an unterminated trace parsed as JSON")
    print("trace_format passed: {} events, {} threads".format(len(events), len(names)))


def main(argv):
    if len(argv) == 3 and argv[1] == "--format":
        format_test(argv[2])
        return 0
    for path in argv[1:]:
        names = check(load(path))
        print("{}: ok, threads {}".format(path, sorted(names.values())))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
