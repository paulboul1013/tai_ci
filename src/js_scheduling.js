// D5: timers and asynchronous XMLHttpRequest, evaluated after the frozen
// tests/reference/runtime.js and js_prelude.js. The frozen browser.py still
// has the Python half (setTimeout, setInterval, clearInterval and the
// asynchronous branch of XMLHttpRequest_send), but commit 7d536e0 of the
// original project dropped the JavaScript half. The timers below are that
// commit's parent's SCHEDULING_RUNTIME_JS (tests/fixtures/
// scheduling_runtime_7d536e0.js); its XMLHttpRequest also replaced the
// constructor and synchronous send, which here stay as the frozen runtime.js
// has them, so only the asynchronous branch is added.

// setTimeout and setInterval share one monotonically increasing handle source.
// This avoids reusing an ID just because an earlier callback was deleted.
var NEXT_TIMER_HANDLE = 0;
var SET_TIMEOUT_REQUESTS = {};
var SET_INTERVAL_REQUESTS = {};

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

// Asynchronous requests are numbered when sent; the frozen constructor has
// no handle. A request that fails never reaches JS again (dropXHR).
var XHR_REQUESTS = {};
var NEXT_XHR_HANDLE = 0;

XMLHttpRequest.prototype.open = function (method, url, is_async) {
    this.is_async = !!is_async;
    this.method = method;
    this.url = url;
};

XMLHttpRequest.prototype.send = function (body) {
    if (body == undefined) {
        body = null;
    }
    if (!this.is_async) {
        this.responseText = call_python(
            "XMLHttpRequest_send", this.method, this.url, body);
        return;
    }
    var handle = NEXT_XHR_HANDLE++;
    XHR_REQUESTS[handle] = this;
    try {
        call_python("XMLHttpRequest_send", this.method, this.url, body,
                    true, handle);
    } catch (error) {
        delete XHR_REQUESTS[handle];
        throw error;
    }
};

function runXHROnload(body, handle) {
    var obj = XHR_REQUESTS[handle];
    delete XHR_REQUESTS[handle];
    if (!obj) return;

    var evt = new Event("load");
    obj.responseText = body;
    if (obj.onload) obj.onload(evt);
}

function dropXHR(handle) {
    delete XHR_REQUESTS[handle];
}
