REBOL [
    System: "REBOL [R3] Language Interpreter and Run-time Environment"
    Title: "REBOL 3 Boot Base: Other Definitions"
    Rights: {
        Copyright 2012 REBOL Technologies
        REBOL is a trademark of REBOL Technologies
    }
    License: {
        Licensed under the Apache License, Version 2.0
        See: http://www.apache.org/licenses/LICENSE-2.0
    }
    Description: {
        This code is evaluated just after actions, natives, sysobj, and
        other lower level definitions. This file intializes a minimal working
        environment that is used for the rest of the boot.
    }
    Note: {
        Any exported SET-WORD!s must be themselves "top level". This hampers
        procedural code here that would like to use tables to avoid repeating
        itself.  This means variadic approaches have to be used that quote
        SET-WORD!s living at the top level, inline after the function call.
    }
]


; Words for BLANK! and BAR!, for those who don't like symbols

blank: _
bar: '|

next: specialize 'skip [offset: 1]
back: specialize 'skip [offset: -1]


eval proc [
    {Make type testing functions (variadic to quote "top-level" words)}
    :set-word... [set-word! <...>]
    <local>
        set-word type-name tester meta
][
    while [any-value? set-word: take set-word...] [
        type-name: append (head clear find (spelling-of set-word) {?}) "!"
        tester: typechecker (get bind (to word! type-name) set-word)
        set set-word :tester

        ; The TYPECHECKER generator doesn't have make meta information by
        ; default, so it leaves it up to the user code.  Note REDESCRIBE is
        ; not defined yet, so this just makes the meta object directly.
        ;
        meta: copy system/standard/function-meta
        meta/description: form reduce [
            {Returns TRUE if the value is a} type-name
        ]
        meta/return-type: [logic!]
        set-meta :tester meta 
    ]
]
    ; This list consumed by the variadic evaluation, up to the | barrier
    ; Each makes a specialization, `XXX: TYPECHECKER XXX!`.  A special
    ; generator is used vs. something like a specialization of a HAS-TYPE?
    ; function...because the generated dispatcher can be more optimized...
    ; and type checking is quite common.
    ;
    blank?:
    bar?:
    lit-bar?:
    logic?:
    integer?:
    decimal?:
    percent?:
    money?:
    char?:
    pair?:
    tuple?:
    time?:
    date?:
    word?:
    set-word?:
    get-word?:
    lit-word?:
    refinement?:
    issue?:
    binary?:
    string?:
    file?:
    email?:
    url?:
    tag?:
    bitset?:
    image?:
    vector?:
    block?:
    group?:
    path?:
    set-path?:
    get-path?:
    lit-path?:
    map?:
    datatype?:
    typeset?:
    function?:
    varargs?:
    object?:
    frame?:
    module?:
    error?:
    port?:
    gob?:
    event?:
    handle?:
    struct?:
    library?:

    ; These typesets are predefined during bootstrap.  REDESCRIBE is not
    ; defined yet, so decide if it's worth it to add descriptions later
    ; e.g. [{Return TRUE if value is } summary {.}]

    any-string?: ;-- "any type of string"
    any-word?: ;-- "any type of word"
    any-path?: ;-- "any type of path"
    any-context?: ;-- "any type of context"
    any-number?: ;-- "a number (integer or decimal)"
    any-series?: ;-- "any type of series"
    any-scalar?: ;-- "any type of scalar"
    any-array?: ;-- "a series of Rebol values"
|


; PROBE is a good early function to have handy for debugging all the rest (!)
;
probe: func [
    {Debug print a molded value and returns that same value.}
    return: [<opt> any-value!]
        {Same as the input value.}
    value [<opt> any-value!]
        {Value to display.}
][
    print mold :value
    :value
]


dump: proc [
    {Show the name of a value (or block of expressions) with the value itself}
    'value
    <local>
        dump-one item
][
    dump-one: proc [item][
        case [
            string? item [
                print ["---" item "---"] ;-- label it
            ]

            word? item [
                print [to set-word! item "=>" mold get item]
            ]

            path? item [
                print [to set-path! item "=>" mold get item]
            ]

            group? item [
                trap/with [
                    print [item "=>" mold eval item]
                ] func [error] [
                    print [item "=!!!=>" mold error]
                ]
            ]

            true [
                fail [
                    "Item not WORD!, PATH!, or GROUP! in DUMP." item
                ]
            ]
        ]
    ]

    either block? value [
        for-each item value [dump-one item]
    ][
        dump-one value
    ]
]


eval proc [
    {Make reflector functions (variadic to quote "top-level" words)}
    :set-word... [set-word! <...>]
    :divider... [blank! <...>]
    :categories... [string! <...>]
    <local>
        set-word categories name
][
    while [any-value? set-word: take set-word...] [
        take divider... ;-- so it doesn't look like we're setting to a string
        categories: take categories...

        ; extract XXX string from XXX-OF
        name: head clear find (spelling-of set-word) {-of}

        set set-word make function! compose/deep [
            [
                (ajoin [{Returns a copy of the } name { of a } categories {.}])
                value [any-value!]
            ][
                reflect :value (to lit-word! name)
            ]
        ]
    ]
]
    spec-of: _ {function, object, or module}
    body-of: _ {function or module} ; %mezz-func.r overwrites
    words-of: _ {function, object, or module}
    values-of: _ {object or module}
    types-of: _ {function}
    addr-of: _ {struct or callback}
    title-of: _ {function} ; should work for module
|


decode-url: _ ; set in sys init

r3-legacy*: _ ; set in %mezz-legacy.r

; used only by Ren-C++ as a test of how to patch the lib context prior to
; boot at the higher levels.
test-rencpp-low-level-hook: _

internal!: make typeset! [
    handle!
]

immediate!: make typeset! [
    ; Does not include internal datatypes
    blank! logic! any-scalar! date! any-word! datatype! typeset! event!
]

system/options/result-types: make typeset! [
    immediate! any-series! bitset! image! object! map! gob!
]


ok?: func [
    "Returns TRUE on all values that are not ERROR!"
    value [<opt> any-value!]
][
    not error? :value
]

; Convenient alternatives for readability
;
neither?: :nand?
both?: :and?
