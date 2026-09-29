"""JS/DOM scenarios shared by the Python oracle probe and the native differential.

Each case parses ``html`` into a document, creates one JavaScript context for
it and runs ``steps`` in order. Step forms:

    ("js", code)                 run code as a global script; record its value
    ("dispatch", type, selector) dispatch an event at the first element in
                                 document order matching selector
    ("raf",)                     run one animation frame's RAF callbacks

Optional ``url`` sets the document URL (used by document.cookie).

The JavaScript side of a "js" step is ``step_source(code)``; both runners use
it verbatim so the value encoding is engine independent. Scenario pages avoid
ids that name engine-specific globals (for example ``dukpy``).
"""

import json

# Runs code through indirect eval so var/function declarations become globals
# like a classic script, then encodes the completion value as JSON. Node
# wrappers become {"node": handle}; undefined, non-finite numbers and functions
# get tagged objects because JSON has no spelling for them.
_STEP_WRAPPER = r"""(function (src) {
    function describe(v) {
        if (v === undefined) return {"undefined": true};
        if (v === null || typeof v === "boolean" || typeof v === "string")
            return v;
        if (typeof v === "number")
            return isFinite(v) ? v : {"number": String(v)};
        if (typeof v === "function") return {"function": true};
        if (v instanceof Node) return {"node": v.handle};
        if (Array.isArray(v)) return v.map(describe);
        var keys = Object.keys(v).sort();
        var out = {};
        for (var i = 0; i < keys.length; i++) out[keys[i]] = describe(v[keys[i]]);
        return {"object": out};
    }
    try {
        return JSON.stringify({"value": describe((0, eval)(src))});
    } catch (e) {
        return JSON.stringify({"error": {"name": String(e && e.name),
                                         "message": String(e && e.message)}});
    }
})(%s)"""


def step_source(code):
    # json.dumps escapes U+2028/U+2029 with ensure_ascii, so the literal is
    # valid JavaScript as well as JSON.
    return _STEP_WRAPPER % json.dumps(code, ensure_ascii=True)


CASES = [
    # --- scheduling APIs as the frozen runtime actually exposes them -------
    {
        "name": "scheduling_globals",
        "html": "<p>x</p>",
        "steps": [
            ("js", "[typeof setTimeout, typeof setInterval, typeof clearInterval,"
                   " typeof requestAnimationFrame, typeof XMLHttpRequest,"
                   " typeof fetch]"),
            ("js", "setTimeout(function () {}, 0)"),
            ("js", "setInterval(function () {}, 10)"),
            ("js", "clearInterval(1)"),
        ],
    },
    {
        "name": "xhr_open",
        "html": "<p>x</p>",
        "steps": [
            ("js", "var xa = new XMLHttpRequest(); xa.open('GET', '/a', true)"),
            ("js", "[xa.method, xa.url, xa.responseText]"),
            ("js", "var xs = new XMLHttpRequest(); xs.open('POST', '/s', false);"
                   " [xs.method, xs.url, xs.responseText]"),
            ("js", "var xn = new XMLHttpRequest(); xn.open('GET', '/n');"
                   " [xn.method, xn.url]"),
        ],
    },

    # --- log -----------------------------------------------------------------
    {
        "name": "log_values",
        "html": "<p>x</p>",
        "steps": [
            ("js", "log('hello'); log(''); log(42); log(1.5); log(true);"
                   " log(null); log([1, 'a'])"),
        ],
    },

    # --- event dispatch -------------------------------------------------------
    {
        "name": "listener_order_and_bubbling",
        "html": "<div id=outer><p id=mid><button id=b>go</button></p></div>",
        "steps": [
            ("js", "b.addEventListener('click', function (e) {"
                   " log('b1 ' + e.type + ' ' + e.target.id + ' '"
                   " + e.currentTarget.id + ' ' + (this.id)); });"
                   " b.addEventListener('click', function () { log('b2'); });"
                   " mid.addEventListener('click', function (e) {"
                   " log('mid ' + e.target.id + ' ' + e.currentTarget.id); });"
                   " outer.addEventListener('click', function () { log('outer'); });"
                   " outer.addEventListener('keydown', function () { log('key'); })"),
            ("dispatch", "click", "#b"),
            ("dispatch", "click", "#mid"),
            ("dispatch", "keydown", "#b"),
            ("dispatch", "submit", "#b"),
        ],
    },
    {
        "name": "prevent_and_stop",
        "html": "<div id=outer><a id=link href=/x>go</a></div>",
        "steps": [
            ("js", "link.addEventListener('click', function (e) {"
                   " e.preventDefault(); log('link'); });"
                   " outer.addEventListener('click', function () { log('outer'); })"),
            ("dispatch", "click", "#link"),
            ("js", "link.addEventListener('keydown', function (e) {"
                   " e.stopPropagation(); log('stop'); });"
                   " link.addEventListener('keydown', function () {"
                   " log('same node still runs'); });"
                   " outer.addEventListener('keydown', function () {"
                   " log('outer keydown'); })"),
            ("dispatch", "keydown", "#link"),
            ("js", "outer.addEventListener('submit', function (e) {"
                   " e.preventDefault(); })"),
            ("dispatch", "submit", "#link"),
        ],
    },
    {
        "name": "listener_throws",
        "html": "<div id=outer><button id=b>go</button></div>",
        "steps": [
            ("js", "b.addEventListener('click', function (e) {"
                   " log('first'); e.preventDefault(); });"
                   " b.addEventListener('click', function () {"
                   " throw Error('boom'); });"
                   " b.addEventListener('click', function () { log('third'); });"
                   " outer.addEventListener('click', function () { log('outer'); })"),
            ("dispatch", "click", "#b"),
            ("js", "outer.addEventListener('keydown', function () {"
                   " undefined_function(); });"
                   " outer.addEventListener('keydown', function () {"
                   " log('after reference error'); })"),
            ("dispatch", "keydown", "#b"),
        ],
    },
    {
        "name": "listener_mutates_during_dispatch",
        "html": "<div id=outer><section id=mid><button id=b>go</button>"
                "</section></div>",
        "steps": [
            ("js", "b.addEventListener('click', function () {"
                   " outer.removeChild(mid); log('removed'); });"
                   " mid.addEventListener('click', function () {"
                   " log('detached mid ' + typeof mid); });"
                   " outer.addEventListener('click', function () {"
                   " log('outer'); })"),
            ("dispatch", "click", "#b"),
            ("js", "outer.addEventListener('keydown', function () {"
                   " outer.setAttribute('id', 'renamed');"
                   " log(typeof outer + ' ' + typeof renamed); })"),
            ("dispatch", "keydown", "#outer"),
            ("dispatch", "keydown", "#renamed"),
        ],
    },
    {
        "name": "listener_on_detached_node",
        "html": "<div id=root></div>",
        "steps": [
            ("js", "var d = document.createElement('span');"
                   " d.addEventListener('click', function () { log('span'); });"
                   " root.addEventListener('click', function () { log('root'); });"
                   " root.appendChild(d); d"),
            ("dispatch", "click", "span"),
        ],
    },

    # --- ID globals -----------------------------------------------------------
    {
        "name": "id_globals_duplicates_and_reserved",
        "html": "<p id=x>first</p><p id=x>second</p>"
                "<div id=document>d</div><div id=Node>n</div>"
                "<div id=window>w</div><div id=log>l</div><div id=Event>e</div>"
                "<div id=LISTENERS>ls</div><div id=ok>ok</div>",
        "steps": [
            ("js", "x.innerHTML"),
            ("js", "[typeof document.querySelectorAll, typeof Node,"
                   " window === this, typeof log, typeof Event,"
                   " Array.isArray(LISTENERS), typeof LISTENERS, ok.innerHTML]"),
            ("js", "Object.keys(ID_GLOBALS).sort()"),
        ],
    },
    {
        "name": "id_globals_script_variable_wins",
        "html": "<div id=holder></div>",
        "steps": [
            ("js", "var mine = 5; holder.innerHTML = '<b id=mine>b</b>'; mine"),
            ("js", "holder.innerHTML = ''; mine"),
        ],
    },
    {
        "name": "id_globals_lifecycle",
        "html": "<div id=holder><span id=foo>f</span></div>",
        "steps": [
            ("js", "var old = foo; holder.removeChild(old);"
                   " [typeof foo, 'foo' in window]"),
            ("js", "holder.appendChild(old); [foo === old, foo.handle === old.handle]"),
            ("js", "foo = 3; holder.removeChild(old); foo"),
            ("js", "holder.appendChild(old); foo"),
        ],
    },
    {
        "name": "id_globals_set_attribute",
        "html": "<div id=a>a</div>",
        "steps": [
            ("js", "var keep = a; keep.setAttribute('ID', 'z');"
                   " [typeof a, typeof z, z.handle === keep.handle]"),
            ("js", "[keep.getAttribute('ID'), keep.getAttribute('id'), keep.id]"),
            ("js", "keep.id = 'y'; [typeof z, typeof y, keep.id]"),
            ("js", "keep.id = ''; [typeof y, keep.id, Object.keys(ID_GLOBALS)]"),
        ],
    },
    {
        "name": "id_globals_detached",
        "html": "<div id=holder></div>",
        "steps": [
            ("js", "var det = document.createElement('div'); det.id = 'late';"
                   " var inner = document.createElement('i'); inner.id = 'deep';"
                   " det.appendChild(inner); [typeof late, typeof deep]"),
            ("js", "holder.appendChild(det); [typeof late, typeof deep,"
                   " late.handle === det.handle]"),
        ],
    },

    # --- attributes and accessors ---------------------------------------------
    {
        "name": "attributes",
        "html": "<div id=a class=c title='' data-x=1>t</div>",
        "steps": [
            ("js", "var el = a; [el.getAttribute('class'), el.getAttribute('title'),"
                   " el.getAttribute('missing'), el.getAttribute('CLASS'),"
                   " el.getAttribute('data-x')]"),
            ("js", "el.setAttribute('Data-Y', 7); el.getAttribute('data-y')"),
            ("js", "el.setAttribute('n', null); el.getAttribute('n')"),
            ("js", "el.setAttribute(true, false); el.getAttribute('true')"),
            ("js", "el.getAttribute(5)"),
            ("js", "el.style = 'color: red'; [el.style, el.getAttribute('style')]"),
            ("js", "el.style = 12; el.style"),
            ("js", "el.id = 42; [el.id, typeof a, typeof window['42']]"),
            ("js", "el.setAttribute('x', undefined)"),
            ("js", "el.id = null"),
        ],
    },
    {
        "name": "children_and_handles",
        "html": "<ul id=list>text<li>1</li> <li class=b>2</li>tail<li>3</li></ul>"
                "<p id=later>p</p>",
        "steps": [
            ("js", "list.children.map(function (n) { return n.innerHTML; })"),
            ("js", "document.querySelectorAll('li')"),
            ("js", "document.querySelectorAll('p')"),
            ("js", "[typeof list.parentNode, typeof list.tagName,"
                   " typeof list.nodeType, typeof document.body]"),
            ("js", "list.children[0] === list.children[0]"),
            ("js", "document.querySelectorAll('ul .b')[0].innerHTML"),
            ("js", "document.querySelectorAll('nothing').length"),
        ],
    },
    {
        "name": "unknown_handles",
        "html": "<div id=a>a</div>",
        "steps": [
            ("js", "new Node(999).getAttribute('x')"),
            ("js", "new Node(999).setAttribute('x', 'y')"),
            ("js", "a.appendChild(new Node(999))"),
            ("js", "new Node(999).children"),
            ("js", "new Node(999).innerHTML"),
            ("js", "a.appendChild({})"),
            ("js", "a.removeChild(null)"),
        ],
    },

    # --- mutation ---------------------------------------------------------------
    {
        "name": "create_element",
        "html": "<div id=root></div>",
        "steps": [
            ("js", "var e1 = document.createElement('DIV'); e1.outerHTML"),
            ("js", "document.createElement('İ').outerHTML"),
            ("js", "document.createElement('Straße').outerHTML"),
            ("js", "document.createElement(5).outerHTML"),
            ("js", "var e2 = document.createElement('p');"
                   " [e2.children.length, e2.innerHTML, e2.getAttribute('id')]"),
            ("js", "root.appendChild(e1) === e1"),
        ],
    },
    {
        "name": "append_and_move",
        "html": "<div id=one><b id=bold>b</b><i>i</i></div><div id=two></div>",
        "steps": [
            ("js", "two.appendChild(bold); [one.innerHTML, two.innerHTML]"),
            ("js", "two.appendChild(bold); two.innerHTML"),
            ("js", "one.appendChild(two); document.querySelectorAll('div').length"),
            ("js", "bold.appendChild(one)"),
            ("js", "one.appendChild(one)"),
        ],
    },
    {
        "name": "insert_before",
        "html": "<ol id=list><li id=l1>1</li><li id=l2>2</li><li id=l3>3</li></ol>"
                "<p id=other>o</p>",
        "steps": [
            ("js", "list.insertBefore(l3, l1); list.innerHTML"),
            ("js", "list.insertBefore(l1, null); list.innerHTML"),
            ("js", "list.insertBefore(l2, l2) === l2; list.innerHTML"),
            ("js", "list.insertBefore(other, l3); list.children.length"),
            ("js", "list.insertBefore(l1, document.createElement('x'))"),
            ("js", "l1.insertBefore(list, null)"),
            ("js", "list.insertBefore(l2)"),
            ("js", "list.insertBefore(l2, undefined)"),
            ("js", "list.insertBefore(list, l1)"),
        ],
    },
    {
        "name": "remove_child",
        "html": "<div id=p><span id=c>c</span><em id=e>e</em></div><div id=q></div>",
        "steps": [
            ("js", "var cc = c; var r = p.removeChild(cc); [r === cc, r.handle,"
                   " p.innerHTML, typeof c]"),
            ("js", "p.removeChild(r)"),
            ("js", "q.removeChild(e)"),
            ("js", "r.outerHTML"),
            ("js", "q.appendChild(r); [q.innerHTML, typeof c]"),
        ],
    },

    # --- serialization -----------------------------------------------------------
    {
        "name": "serialize",
        "html": "<div id=s b=1 a=2 b=3 q='x\"y' amp='a&amp;b' ent=\"a&quot;b\">"
                "t &lt; &amp; &gt; &quot; ' <!-- gone --><img src=x.png>"
                "<input disabled><br/>after<script>if (a < b && c > d) {}</script>"
                "<span EMPTY>e</span></div>",
        "steps": [
            ("js", "s.outerHTML"),
            ("js", "s.innerHTML"),
            ("js", "document.querySelectorAll('span')[0].outerHTML"),
            ("js", "document.querySelectorAll('html')[0].outerHTML"),
        ],
    },

    # --- innerHTML ---------------------------------------------------------------
    {
        "name": "inner_html_set",
        "html": "<div id=box><b id=gone>g</b></div>",
        "steps": [
            ("js", "var oldb = gone; box.innerHTML = '<p id=fresh>new <i>x</i></p>';"
                   " [box.innerHTML, typeof gone, typeof fresh, oldb.outerHTML]"),
            ("js", "box.innerHTML = 5; box.innerHTML"),
            ("js", "box.innerHTML = null"),
            ("js", "box.innerHTML"),
            ("js", "box.innerHTML = 'a<body>b</body>c<body><i>last</i></body>';"
                   " box.innerHTML"),
            ("js", "box.innerHTML = '<head><title>t</title></head>text';"
                   " box.innerHTML"),
            ("js", "box.innerHTML = '';"
                   " [box.innerHTML, box.children.length]"),
            ("js", "box.innerHTML = 'x &amp; &lt;y&gt;'; box.innerHTML"),
            ("js", "oldb.innerHTML = '<u>detached</u>'; oldb.outerHTML"),
            ("js", "box.innerHTML = { toString: function () { return '<s>o</s>'; } };"
                   " box.innerHTML"),
            ("js", "box.outerHTML = 'x'; box.outerHTML"),
        ],
    },

    # --- requestAnimationFrame ----------------------------------------------------
    {
        "name": "raf_batches",
        "html": "<div id=c>0</div>",
        "steps": [
            ("js", "requestAnimationFrame(function () { log('a');"
                   " requestAnimationFrame(function () { log('nested'); }); });"
                   " requestAnimationFrame(function () { log('b');"
                   " c.innerHTML = '1'; })"),
            ("raf",),
            ("raf",),
            ("raf",),
        ],
    },
    {
        "name": "raf_throws",
        "html": "<div id=c>0</div>",
        "steps": [
            ("js", "requestAnimationFrame(function () { log('one'); });"
                   " requestAnimationFrame(function () { throw Error('raf boom'); });"
                   " requestAnimationFrame(function () { log('dropped'); })"),
            ("raf",),
            ("raf",),
            ("js", "RAF_LISTENERS.length"),
        ],
    },

    # --- document.cookie (no network; one cookie per host) -----------------------
    {
        "name": "cookie",
        "url": "http://example.test/page",
        "html": "<p>c</p>",
        "steps": [
            ("js", "document.cookie"),
            ("js", "document.cookie = 'theme=dark; SameSite=Lax; Path=/';"
                   " document.cookie"),
            ("js", "document.cookie = 'malformed'; document.cookie"),
            ("js", "document.cookie = 'x=1; HttpOnly'; document.cookie"),
            ("js", "document.cookie = 'theme=light'; document.cookie"),
            ("js", "document.cookie = 'theme=old;"
                   " Expires=Thu, 01 Jan 1970 00:00:00 GMT'; document.cookie"),
            ("js", "document.cookie = 42; document.cookie"),
        ],
    },
    {
        "name": "cookie_http_only_existing",
        "url": "http://example.test/",
        "cookie_jar": {"example.test": ["sid=secret", {"httponly": "true"}]},
        "html": "<p>c</p>",
        "steps": [
            ("js", "document.cookie"),
            ("js", "document.cookie = 'sid=stolen'; document.cookie"),
        ],
    },
]
