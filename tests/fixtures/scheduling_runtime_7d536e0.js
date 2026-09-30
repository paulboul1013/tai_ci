// SCHEDULING_RUNTIME_JS of the original project's browser.py at commit
// e5e8ed449c98fd69140250f9d23c3eb80d3ed11d (7d536e0^), verbatim. Commit 7d536e0
// removed it when requestAnimationFrame moved into runtime.js; the frozen
// tests/reference/browser.py still has the Python half. The oracle probes
// evaluate this after runtime.js, as that JSContext did (D5).
// setTimeout and setInterval share one monotonically increasing handle source.
// This avoids reusing an ID just because an earlier callback was deleted.
NEXT_TIMER_HANDLE = 0;
SET_TIMEOUT_REQUESTS = {};
SET_INTERVAL_REQUESTS = {};

function setTimeout(callback, time_delta) {
    var handle = NEXT_TIMER_HANDLE++;
    SET_TIMEOUT_REQUESTS[handle] = callback;
    call_python("setTimeout", handle, time_delta);
    return handle;
}

function runSetTimeout(handle) {
    var callback = SET_TIMEOUT_REQUESTS[handle];
    if (callback === undefined) return;

    // A timeout is one-shot. Delete it before invoking user code so the callback
    // can schedule another timer without retaining the completed callback.
    delete SET_TIMEOUT_REQUESTS[handle];
    callback();
}

function setInterval(callback, time_delta) {
    var handle = NEXT_TIMER_HANDLE++;
    SET_INTERVAL_REQUESTS[handle] = callback;
    call_python("setInterval", handle, time_delta);
    return handle;
}

function clearInterval(handle) {
    // Deleting the JavaScript callback makes already-queued interval tasks safe:
    // runSetInterval becomes a no-op even if one crossed the queue boundary.
    delete SET_INTERVAL_REQUESTS[handle];
    call_python("clearInterval", handle);
}

function runSetInterval(handle) {
    var callback = SET_INTERVAL_REQUESTS[handle];
    if (callback === undefined) return;
    callback();
}

RAF_LISTENERS = [];

function requestAnimationFrame(callback) {
    RAF_LISTENERS.push(callback);
    call_python("requestAnimationFrame");
}

function runRAFHandlers() {
    // Move this frame's callbacks out before running them. A callback that
    // requests another animation frame therefore lands in the fresh list and
    // cannot run recursively in the current frame.
    var handlers_copy = RAF_LISTENERS;
    RAF_LISTENERS = [];

    for (var i = 0; i < handlers_copy.length; i++) {
        handlers_copy[i]();
    }
}

XHR_REQUESTS = {};

XMLHttpRequest = function() {
    this.handle = Object.keys(XHR_REQUESTS).length;
    XHR_REQUESTS[this.handle] = this;
    this.is_async = false;
    this.method = "GET";
    this.url = "";
    this.responseText = "";
    this.onload = null;
};

XMLHttpRequest.prototype.open = function(method, url, is_async) {
    this.is_async = !!is_async;
    this.method = method;
    this.url = url;
};

XMLHttpRequest.prototype.send = function(body) {
    if (body === undefined) body = null;
    var out = call_python(
        "XMLHttpRequest_send",
        this.method, this.url, body, this.is_async, this.handle
    );
    if (!this.is_async) this.responseText = out;
    return out;
};

function runXHROnload(body, handle) {
    var obj = XHR_REQUESTS[handle];
    if (!obj) return;

    var evt = new Event("load");
    obj.responseText = body;
    if (obj.onload) obj.onload(evt);
}
