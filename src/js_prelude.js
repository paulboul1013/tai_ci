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

// D10: a throwing animation frame callback is reported and skipped, like a
// real browser; the rest of the batch still runs. Python drops the rest.
// As in the frozen version, callbacks queued meanwhile wait for the next frame.
function runRAFHandlers() {
    var handlers = RAF_LISTENERS;
    RAF_LISTENERS = [];

    for (var i = 0; i < handlers.length; i++) {
        try {
            handlers[i]();
        } catch (error) {
            call_python("raf_error", error);
        }
    }
}

// D14: common DOM API that runtime.js lacks, following real browsers.
// <html>.parentNode is document (the bridge reports it as -1), tagName is
// upper case, and textContent includes script and style text.
Object.defineProperty(Node.prototype, "parentNode", {
    get: function () {
        var handle = call_python("parentNode", this.handle);
        if (handle === null) return null;
        return handle === -1 ? document : new Node(handle);
    }
});

Object.defineProperty(Node.prototype, "parentElement", {
    get: function () {
        var parent = this.parentNode;
        return parent === document ? null : parent;
    }
});

Object.defineProperty(Node.prototype, "tagName", {
    get: function () {
        var tag = call_python("tagName", this.handle);
        return tag === null ? undefined : tag.toUpperCase();
    }
});

Object.defineProperty(Node.prototype, "textContent", {
    get: function () {
        return call_python("textContent_get", this.handle);
    },
    set: function (value) {
        call_python("textContent_set", this.handle,
                    value == null ? "" : String(value));
    }
});

Object.defineProperty(Node.prototype, "className", {
    get: function () {
        return this.getAttribute("class");
    },
    set: function (value) {
        this.setAttribute("class", String(value));
    }
});

function ClassList(node) {
    this.node = node;
}

// Tokens split on ASCII whitespace only, without duplicates.
ClassList.prototype.tokens = function () {
    var tokens = [];
    this.node.className.split(/[\t\n\f\r ]+/).forEach(function (token) {
        if (token !== "" && tokens.indexOf(token) < 0) tokens.push(token);
    });
    return tokens;
};

// Like DOMTokenList's update steps: an element without a class attribute
// keeps none when the token set stays empty. getAttribute reads a missing
// attribute as "", so an empty class attribute is treated the same.
ClassList.prototype.update = function (tokens) {
    if (tokens.length === 0 && this.node.className === "") return;
    this.node.className = tokens.join(" ");
};

ClassList.prototype.contains = function (token) {
    return this.tokens().indexOf(String(token)) >= 0;
};

ClassList.prototype.add = function () {
    var tokens = this.tokens();
    for (var i = 0; i < arguments.length; i++) {
        var token = String(arguments[i]);
        if (tokens.indexOf(token) < 0) tokens.push(token);
    }
    this.update(tokens);
};

ClassList.prototype.remove = function () {
    var removed = Array.prototype.map.call(arguments, String);
    this.update(this.tokens().filter(function (token) {
        return removed.indexOf(token) < 0;
    }));
};

ClassList.prototype.toggle = function (token, force) {
    var present = this.contains(token);
    var wanted = force === undefined ? !present : Boolean(force);
    if (wanted && !present) this.add(token);
    if (!wanted && present) this.remove(token);
    return wanted;
};

Object.defineProperty(Node.prototype, "classList", {
    get: function () {
        return new ClassList(this);
    }
});

document.getElementById = function (id) {
    var handle = call_python("getElementById", String(id));
    return handle === null ? null : new Node(handle);
};

document.querySelector = function (selector) {
    var matches = document.querySelectorAll(selector);
    return matches.length ? matches[0] : null;
};
