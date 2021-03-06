REBOL [
    System: "REBOL [R3] Language Interpreter and Run-time Environment"
    Title: "Generate extention native header files"
    File: %make-ext-native.r ;-- used by EMIT-HEADER to indicate emitting script
    Rights: {
        Copyright 2012 REBOL Technologies
        REBOL is a trademark of REBOL Technologies
    }
    License: {
        Licensed under the Apache License, Version 2.0
        See: http://www.apache.org/licenses/LICENSE-2.0
    }
    Author: "Shixin Zeng <szeng@atronixengineering.com>"
    Needs: 2.100.100
]

do %common.r
do %common-emitter.r
do %form-header.r
do %common-parsers.r
do %native-emitters.r ;for emit-native-proto and emit-include-params-macro

r3: system/version > 2.100.0

args: parse-args system/options/args
output-dir: fix-win32-path to file! any [args/OUTDIR %../]
mkdir/deep output-dir/include

process: func [
    file
][
    if verbose [probe [file]]

    source.text: read the-file: file
    ;print ["source:" to string! source.text]
    if r3 [source.text: deline to-string source.text]
    proto-parser/emit-proto: :emit-native-proto
    proto-parser/process source.text
]

boot-modules: copy []

either blank? args/MODULE [
    ;read process all builtin modules in file-base/modules
    ;used by make-make.r
    file-base: load %file-base.r
    for-each [builtin? m-name m-src] file-base/modules [
        if '+ = builtin? [
            append boot-modules reduce [to string! m-name m-src]
        ]
    ]
][
    ;process one module at a time
    ;used by CMake
    boot-modules: reduce [args/MODULE args/SRC]
]

for-each [m-name c-src] boot-modules [
    c-src: fix-win32-path to file! c-src
    print ["building" m-name "from" c-src]
    l-m-name: lowercase copy m-name
    u-m-name: uppercase copy m-name

    verbose: false

    unsorted-buffer: make string! 20000

    c-natives: make block! 128

    proto-count: 0
    process c-src

    native-list: load unsorted-buffer
    word-list: copy []
    export-list: copy []
    error-list: copy []
    num-native: 0
    unless parse native-list [
        while [
            set w set-word! [
                'native block!
                | 'native/body 2 block!
                | [
                    'native/export block!
                    | 'native/export/body 2 block!
                    | 'native/body/export 2 block!
                ] (append export-list to word! w)
            ] (++ num-native)
            | remove [quote new-words: set words block! (append word-list words)]
            | remove [quote new-errors: set errors block! (append error-list errors)]
        ]
    ][
        fail rejoin ["failed to parse" mold native-list ", current word-list:" mold word-list]
    ]
    ;print ["specs:" mold native-list]
    word-list: unique word-list
    spec: compose/deep/only [
        REBOL [
            name: (to word! m-name)
            exports: (export-list)
        ]
    ]
    unless empty? word-list [
        append spec compose/only [
            words: (word-list)
        ]
    ]
    unless empty? error-list [
        append spec compose/only [
            errors: (error-list)
        ]
    ]
    append spec native-list
    comp-data: compress data: to-binary mold spec
    ;print ["buf:" to string! data]

    emit-header m-name to file! rejoin [%tmp- l-m-name %.h]
    emit-line ["#define EXT_NUM_NATIVES_" u-m-name space num-native]
    emit-line ["#define EXT_NAT_COMPRESSED_SIZE_" u-m-name space length comp-data]
    emit-line ["const REBYTE Ext_Native_Specs_" m-name "[EXT_NAT_COMPRESSED_SIZE_" u-m-name "] = {"]

    ;-- Convert UTF-8 binary to C-encoded string:
    emit binary-to-c comp-data
    emit-line "};" ;-- EMIT-END would erase the last comma, but there's no extra

    emit-line ["REBNAT Ext_Native_C_Funcs_" m-name "[EXT_NUM_NATIVES_" u-m-name "] = {"]
    for-each item native-list [
        if set-word? item [
            emit-item ["N_" to word! item]
        ]
    ]
    emit-end

    emit-line [ {
    int Module_Init_} m-name {_Core(RELVAL *out)
    ^{
        INIT_} u-m-name {_WORDS;}
	either empty? error-list [ rejoin [ {
		REBARR * arr = Make_Extension_Module_Array(
            Ext_Native_Specs_} m-name {, EXT_NAT_COMPRESSED_SIZE_} u-m-name {,
            Ext_Native_C_Funcs_} m-name {, EXT_NUM_NATIVES_} u-m-name {,
			-1);} ]
        ][
            rejoin [ {
		Ext_} m-name {_Error_Base = Find_Next_Error_Base_Code();
		REBARR * arr = Make_Extension_Module_Array(
            Ext_Native_Specs_} m-name {, EXT_NAT_COMPRESSED_SIZE_} u-m-name {,
            Ext_Native_C_Funcs_} m-name {, EXT_NUM_NATIVES_} u-m-name {,
			Ext_} m-name {_Error_Base);}]
        ] {
        Init_Block(out, arr);

        return 0;
    ^}

    int Module_Quit_} m-name {_Core(void)
    {
        return 0;
    }
    }
    ]

    write-emitted to file! rejoin [output-dir/include/tmp-ext- l-m-name %-last.h]

    ;--------------------------------------------------------------
    ; args
    emit-header "PARAM() and REFINE() Automatic Macros" to file! rejoin [%tmp-ext- l-m-name %-first.h]
    emit-native-include-params-macro native-list

    ;--------------------------------------------------------------
    ; words
    emit-line ["//  Local words"]
    emit-line ["#define NUM_EXT_" u-m-name "_WORDS" space length word-list]

    either empty? word-list [
        emit-line ["#define INIT_" u-m-name "_WORDS"]
    ][
        emit-line ["static const char* Ext_Words_" m-name "[NUM_EXT_" u-m-name "_WORDS] = {"]
        for-next word-list [
            emit-line/indent [ {"} to string! word-list/1 {",} ]
        ]
        emit-end

        emit-line ["static REBSTR* Ext_Canons_" m-name "[NUM_EXT_" u-m-name "_WORDS];"]

        word-seq: 0
        for-next word-list [
            emit-line ["#define" space u-m-name {_WORD_} uppercase to-c-name word-list/1 space
                {Ext_Canons_} m-name {[} word-seq {]}]
            ++ word-seq
        ]
        emit-line ["#define INIT_" u-m-name "_WORDS" space "\"]
        emit-line/indent ["Init_Extension_Words(cast(const REBYTE**, Ext_Words_" m-name "), Ext_Canons_" m-name ", NUM_EXT_" u-m-name "_WORDS)"]
    ]

    ;--------------------------------------------------------------
    ; errors
    emit-line ["//  Local errors"]
    unless empty? error-list [
        emit-line [ {enum Ext_} m-name {_Errors ^{}]
        error-collected: copy []
        for-each [key val] error-list [
            unless set-word? key [
                fail rejoin ["key (" mold key ") must be a set-word!"]
            ]
            if found? find error-collected key [
                fail rejoin ["Duplicate error key" to word! key]
            ]
            append error-collected key
            emit-item/upper rejoin [ {RE_EXT_ENUM_} u-m-name {_} uppercase to-c-name to word! key]
        ]
        emit-end
        emit-line ["static REBINT Ext_" m-name "_Error_Base;"]

        emit-line []
        for-each [key val] error-list [
            emit-line rejoin [ {#define RE_EXT_} u-m-name {_} uppercase to-c-name to word! key
            space
            {(Ext_} m-name {_Error_Base + }
            {RE_EXT_ENUM_} u-m-name {_} uppercase to-c-name to word! key {)}
            ]
        ]
    ]

    write-emitted to file! rejoin [output-dir/include/tmp-ext- l-m-name %-first.h]
]
