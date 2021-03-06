REBOL [
    System: "Ren/C Core Extraction of the Rebol System"
    Title: "Common Routines for Tools"
    Rights: {
        Copyright 2012-2017 Rebol Open Source Contributors
        REBOL is a trademark of REBOL Technologies
    }
    License: {
        Licensed under the Apache License, Version 2.0
        See: http://www.apache.org/licenses/LICENSE-2.0
    }
    Version: 2.100.0
    Needs: 2.100.100
    Purpose: {
        These are some common routines used by the utilities
        that build the system, which are found in %src/tools/
    }
]

;-- !!! BACKWARDS COMPATIBILITY: this does detection on things that have
;-- changed, in order to adapt the environment so that the build scripts
;-- can still work in older as well as newer Rebols.  Thus the detection
;-- has to be a bit "dynamic"

do %r2r3-future.r


spaced-tab: rejoin [space space space space]

tab-char: #"^-" ;-- only GNU Makefiles require this...
tab: does [
    fail "Don't generate files with tabs in them, use SPACED-TAB"
]

to-c-name: function [
    {Take a Rebol value and transliterate it as a (likely) valid C identifier.}

    value [string! block! word!]
        {Will be converted to a string (via REJOIN if BLOCK!)}
    /scope
        {See scope rules: http://stackoverflow.com/questions/228783/}
    word [word!]
        {Either 'global or 'local (defaults global)}
][
    c-chars: charset [
        #"a" - #"z"
        #"A" - #"Z"
        #"0" - #"9"
        #"_"
    ]

    string: either block? :value [rejoin value][form value]

    string: switch/default attempt [to-word string] [
        ; Take care of special cases of singular symbols

        ; Used specifically by t-routine.c to make SYM_ELLIPSIS
        ... [copy "ellipsis"]

        ; Used to make SYM_HYPHEN which is needed by `charset [#"A" - #"Z"]`
        - [copy "hyphen"]

        ; Used by u-dialect apparently
        * [copy "asterisk"]

        ; None of these are used at present, but included in case
        . [copy "period"]
        ? [copy "question"]
        ! [copy "exclamation"]
        + [copy "plus"]
        ~ [copy "tilde"]
        | [copy "bar"]
    ][
        ; If these symbols occur composite in a longer word, they use a
        ; shorthand; e.g. `true?` => `true_q`

        for-each [reb c] [
            -   "_"
            *   "_p"    ; !!! because it symbolizes a (p)ointer in C??
            .   "_"     ; !!! same as hyphen?
            ?   "_q"
            !   "_x"    ; e(x)clamation
            +   "_a"    ; (a)ddition
            ~   "_t"
            |   "_b"

        ][
            replace/all string (form reb) c
        ]

        string
    ]

    if empty? string [
        fail [
            "empty identifier produced by to-c-name for"
            (mold value) "of type" (mold type-of value)
        ]
    ]

    comment [
        ; Don't worry about leading digits at the moment, because currently
        ; the code will do a to-c-name transformation and then often prepend
        ; something to it.

        if find charset [#"0" - #"9"] string/1 [
            fail ["identifier" string "starts with digit in to-c-name"]
        ]
    ]

    for-each char string [
        if char = space [
            ; !!! The way the callers seem to currently be written is to
            ; sometimes throw "foo = 2" kinds of strings and expect them to
            ; be converted to a "C string".  Only check the part up to the
            ; first space for legitimacy then.  :-/
            break
        ]

        unless find c-chars char [
            fail ["Non-alphanumeric or hyphen in" string "in to-c-name"]
        ]
    ]

    unless scope [word: 'global] ; default to assuming global need

    ; Easiest rule is just "never start a global identifier with underscore",
    ; but we check the C rules.  Since currently this routine is sometimes
    ; called to produce a partial name, it may not be a problem if that part
    ; starts with an underscore if something legal will be prepended.  But
    ; there are no instances of that need so better to plant awareness.

    catch [case/all [
        string/1 != "_" [throw string]

        word = 'global [
            fail [
                "global identifiers in C starting with underscore"
                "are reserved for standard library usage"
            ]
        ]

        word = 'local [
            unless find charset [#"A" - #"Z"] value/2 [
                throw string
            ]
            fail [
                "local identifiers in C starting with underscore and then"
                "a capital letter are reserved for standard library usage"
            ]
        ]

        'default [fail "scope word must be 'global or 'local"]
    ]]

    string
]


; http://stackoverflow.com/questions/11488616/
binary-to-c: function [
    {Converts a binary to a string of C source that represents an initializer
    for a character array.  To be "strict" C standard compatible, we do not
    use a string literal due to length limits (509 characters in C89, and
    4095 characters in C99).  Instead we produce an array formatted as
    '{0xYY, ...}' with 8 bytes per line}

    data [binary!]
][
    out: make string! 6 * (length data)
    while [not tail? data] [
        append out spaced-tab

        ;-- grab hexes in groups of 8 bytes
        hexed: enbase/base (copy/part data 8) 16
        data: skip data 8
        for-each [digit1 digit2] hexed [
            append out rejoin [{0x} digit1 digit2 {,} space]
        ]

        take/last out ;-- drop the last space
        if tail? data [
            take/last out ;-- lose that last comma
        ]
        append out newline ;-- newline after each group, and at end
    ]

    ;-- Sanity check (should be one more byte in source than commas out)
    parse out [(comma-count: 0) some [thru "," (++ comma-count)] to end]
    assert [(comma-count + 1) = (length head data)]

    out
]


; !!! WARNING: Bootstrap needs to stay working with R3-Alpha.  So don't
; assume this is safe for using with RETURN...because under R3-Alpha that
; will basically act as a BREAK, returning from the FOR-EACH-RECORD but not
; respecting the intention of the RETURN at the callsite.  (Used to have
; the alarmist name FOR-EACH-RECORD-NO-RETURN, but that was overkill.)
;
for-each-record: procedure [
    {Iterate a table with a header by creating an object for each row}

    'record [word!]
        {Word to set each time to the row made into an object}
    table [block!]
        {Table of values with header block as first element}
    body [block!]
        {Block to evaluate each time}
][
    unless block? first table [
        fail {Table of records does not start with a header block}
    ]

    headings: map-each word first table [
        unless word? word [
            fail [{Heading} word {is not a word}]
        ]
        to-set-word word
    ]

    table: next table

    while [not tail? table] [
        if (length headings) > (length table) [
            fail {Element count isn't even multiple of header count}
        ]

        spec: collect [
            for-each column-name headings [
                keep column-name
                keep compose/only [quote (table/1)]
                table: next table
            ]
        ]

        set record has spec

        do body
    ]

    ; In Ren-C, to return a result this would have to be marked as returning
    ; an optional value...but that syntax would confuse R3-Alpha, which this
    ; has to run under.  So we just don't bother returning a result.
]


find-record-unique: function [
    {Get a record in a table as an object, error if duplicate, blank if absent}
    
    ;; return: [object! blank!]
    table [block!]
        {Table of values with header block as first element}
    key [word!]
        {Object key to search for a match on}
    value
        {Value that the looked up key must be uniquely equal to}
][
    unless find first table key [
        fail [key {not found in table headers:} (first table)]
    ]

    result: _
    for-each-record rec table [
        unless value = select rec key [continue]

        if result [
            fail [{More than one table record matches} key {=} value]
        ]

        result: rec

        ; RETURN won't work when running under R3-Alpha.  We could break, but
        ; walk whole table to verify that it is well-formed.  (Correctness is
        ; more important.)
    ]
    result
]


parse-args: function [
    args ;args in form of "NAME=VALUE"
][
    ret: make block! 4
    args: any [args copy []]
    unless block? args [args: split args [some " "]]
    for-each a args [
        if idx: find a #"=" [
            name: to word! copy/part a (index-of idx) - 1
            value: copy next idx
            append ret reduce [name value]
        ]
    ]
    ret
]

fix-win32-path: func [
    path [file!]
    /local letter colon
][
    if 3 != fourth system/version [return path] ;non-windows system

    drive: first path
    colon: second path

    if all [
        any [
        all [#"A" <= drive #"Z" >= drive] 
        all [#"a" <= drive #"z" >= drive] 
    ]
    #":" = colon
    ][
        insert path #"/"
    remove skip path 2 ;remove ":"
    ]

    path
]

uppercase-of: func [
    {Copying variant of UPPERCASE, also FORMs words}
    value [string! word!]
][
    uppercase form value
]

lowercase-of: func [
    {Copying variant of LOWERCASE, also FORMs words}
    value [string! word!]
][
    lowercase form value
]

propercase: func [value] [uppercase/part (copy value) 1]

propercase-of: func [
    {Make a copy of a string with just the first character uppercase}
    value [string! word!]
][
    propercase form value
]

