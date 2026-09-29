// Native-only additions evaluated after the frozen tests/reference/runtime.js,
// which is embedded unchanged. Everything here is a recorded intentional
// difference from the Python browser (see PORTING_PLAN.md).

// D7: a throwing listener is reported and skipped, like a real browser. The
// remaining listeners on this node, bubbling, and an earlier preventDefault or
// stopPropagation still apply. Python aborts the whole dispatch instead.
Node.prototype.dispatchEvent = function (event) {
    var type = event.type;
    var handle = this.handle;
    var list = (LISTENERS[handle] && LISTENERS[handle][type]) || [];

    event.currentTarget = this;

    for (var i = 0; i < list.length; i++) {
        try {
            list[i].call(this, event);
        } catch (error) {
            call_python("listener_error", type, error);
        }
    }

    return event.do_default;
};
