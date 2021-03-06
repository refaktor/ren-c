//
//  File: %t-typeset.c
//  Summary: "typeset datatype"
//  Section: datatypes
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2017 Rebol Open Source Contributors
// REBOL is a trademark of REBOL Technologies
//
// See README.md and CREDITS.md for more information.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//=////////////////////////////////////////////////////////////////////////=//
//

#include "sys-core.h"


//
// symbol-to-typeset-bits mapping table
//
// NOTE: Order of symbols is important, because this is used to build a
// list of typeset word symbols ordered relative to their symbol #,
// which lays out the legal unbound WORD! values you can use during
// a MAKE TYPESET! (bound words will be looked up as variables to see
// if they contain a DATATYPE! or a typeset, but general reduction is
// not performed on the block passed in.)
//
// !!! Is it necessary for MAKE TYPESET! to allow unbound words at all,
// or should the typesets be required to be in bound variables?  Should
// clients be asked to pass in only datatypes and typesets, hence doing
// their own reduce before trying to make a typeset out of a block?
//
const struct {
    REBSYM sym;
    REBU64 bits;
} Typesets[] = {
    {SYM_ANY_VALUE_X, TS_VALUE},
    {SYM_ANY_WORD_X, TS_WORD},
    {SYM_ANY_PATH_X, TS_PATH},
    {SYM_ANY_NUMBER_X, TS_NUMBER},
    {SYM_ANY_SCALAR_X, TS_SCALAR},
    {SYM_ANY_SERIES_X, TS_SERIES},
    {SYM_ANY_STRING_X, TS_STRING},
    {SYM_ANY_CONTEXT_X, TS_CONTEXT},
    {SYM_ANY_ARRAY_X, TS_ARRAY},

    {SYM_0, 0}
};


//
//  CT_Typeset: C
//
REBINT CT_Typeset(const RELVAL *a, const RELVAL *b, REBINT mode)
{
    if (mode < 0) return -1;
    return EQUAL_TYPESET(a, b);
}


//
//  Init_Typesets: C
//
// Create typeset variables that are defined above.
// For example: NUMBER is both integer and decimal.
// Add the new variables to the system context.
//
void Init_Typesets(void)
{
    Set_Root_Series(ROOT_TYPESETS, AS_SERIES(Make_Array(40)));

    REBINT n;
    for (n = 0; Typesets[n].sym != 0; n++) {
        REBVAL *value = Alloc_Tail_Array(VAL_ARRAY(ROOT_TYPESETS));

        // Note: the symbol in the typeset is not the symbol of a word holding
        // the typesets, rather an extra data field used when the typeset is
        // in a context key slot to identify that field's name
        //
        Init_Typeset(value, Typesets[n].bits, NULL);

        *Append_Context(Lib_Context, NULL, Canon(Typesets[n].sym)) = *value;
    }
}


//
//  Init_Typeset: C
//
// Name should be set when a typeset is being used as a function parameter
// specifier, or as a key in an object.
//
void Init_Typeset(RELVAL *value, REBU64 bits, REBSTR *opt_name)
{
    VAL_RESET_HEADER(value, REB_TYPESET);
    SET_VAL_FLAG(value, TYPESET_FLAG_NO_LOOKBACK); // default
    INIT_TYPESET_NAME(value, opt_name);
    VAL_TYPESET_BITS(value) = bits;
}


//
//  Update_Typeset_Bits_Core: C
//
// This sets the bits in a bitset according to a block of datatypes.  There
// is special handling by which BAR! will set the "variadic" bit on the
// typeset, which is heeded by functions only.
//
// !!! R3-Alpha supported fixed word symbols for datatypes and typesets.
// Confusingly, this means that if you have said `word!: integer!` and use
// WORD!, you will get the integer type... but if WORD! is unbound then it
// will act as WORD!.  Also, is essentially having "keywords" and should be
// reviewed to see if anything actually used it.
//
REBOOL Update_Typeset_Bits_Core(
    RELVAL *typeset,
    const RELVAL *head,
    REBSPC *specifier
) {
    assert(IS_TYPESET(typeset));
    VAL_TYPESET_BITS(typeset) = 0;

    const RELVAL *item = head;
    if (NOT_END(item) && IS_BLOCK(item)) {
        // Double blocks are a variadic signal.
        if (NOT_END(item + 1))
            fail (Error(RE_MISC));

        item = VAL_ARRAY_AT(item);
        SET_VAL_FLAG(typeset, TYPESET_FLAG_VARIADIC);
    }

    for (; NOT_END(item); item++) {
        const RELVAL *var = NULL;

        if (IS_WORD(item))
            var = Get_Opt_Var_May_Fail(item, specifier);

        if (var == NULL)
            var = item;

        // Though MAKE FUNCTION! at its lowest level attempts to avoid any
        // keywords, there are native-optimized function generators that do
        // use them.  Since this code is shared by both, it may or may not
        // set typeset flags as a parameter.  Default to always for now.
        //
        const REBOOL keywords = TRUE;

        if (
            keywords && IS_TAG(item) && (
                0 == Compare_String_Vals(item, ROOT_ELLIPSIS_TAG, TRUE)
            )
        ) {
            // Notational convenience for variadic.
            // func [x [<...> integer!]] => func [x [[integer!]]]
            //
            SET_VAL_FLAG(typeset, TYPESET_FLAG_VARIADIC);
        }
        else if (
            keywords && IS_TAG(item) && (
                0 == Compare_String_Vals(item, ROOT_TIGHT_TAG, TRUE)
            )
        ) {
            // Makes enfixed first arguments "lazy" and other arguments will
            // use the DO_FLAG_NO_LOOKAHEAD.
            //
            SET_VAL_FLAG(typeset, TYPESET_FLAG_TIGHT);
        }
        else if (
            IS_BAR(item) || (keywords && IS_TAG(item) && (
                0 == Compare_String_Vals(item, ROOT_END_TAG, TRUE)
            ))
        ) {
            // A BAR! in a typeset spec for functions indicates a tolerance
            // of endability.  Notational convenience:
            //
            // func [x [<end> integer!]] => func [x [| integer!]]
            //
            SET_VAL_FLAG(typeset, TYPESET_FLAG_ENDABLE);
        }
        else if (
            IS_BLANK(item) || (keywords && IS_TAG(item) && (
                0 == Compare_String_Vals(item, ROOT_OPT_TAG, TRUE)
            ))
        ) {
            // A BLANK! in a typeset spec for functions indicates a willingness
            // to take an optional.  (This was once done with the "UNSET!"
            // datatype, but now that there isn't a user-exposed unset data
            // type this is not done.)  Still, since REB_MAX_VOID is available
            // internally it is used in the type filtering here.
            //
            // func [x [<opt> integer!]] => func [x [_ integer!]]
            //
            // !!! As with BAR! for variadics, review if this makes sense to
            // allow with `make typeset!` instead of just function specs.
            // Note however that this is required for the legacy compatibility
            // of ANY-TYPE!, which included UNSET! because it was a datatype
            // in R3-Alpha and Rebol2.
            //
            TYPE_SET(typeset, REB_MAX_VOID);
        }
        else if (IS_DATATYPE(var)) {
            TYPE_SET(typeset, VAL_TYPE_KIND(var));
        }
        else if (IS_TYPESET(var)) {
            VAL_TYPESET_BITS(typeset) |= VAL_TYPESET_BITS(var);
        }
        else
            fail (Error_Invalid_Arg_Core(item, specifier));
    }

    return TRUE;
}


//
//  MAKE_Typeset: C
//
void MAKE_Typeset(REBVAL *out, enum Reb_Kind kind, const REBVAL *arg)
{
    assert(kind == REB_TYPESET);

    if (IS_TYPESET(arg)) {
        *out = *arg;
        return;
    }

    if (!IS_BLOCK(arg)) goto bad_make;

    Init_Typeset(out, 0, NULL);
    Update_Typeset_Bits_Core(out, VAL_ARRAY_AT(arg), VAL_SPECIFIER(arg));
    return;

bad_make:
    fail (Error_Bad_Make(REB_TYPESET, arg));
}


//
//  TO_Typeset: C
//
void TO_Typeset(REBVAL *out, enum Reb_Kind kind, const REBVAL *arg)
{
    MAKE_Typeset(out, kind, arg);
}


//
//  Typeset_To_Array: C
//
// Converts typeset value to a block of datatypes.
// No order is specified.
//
REBARR *Typeset_To_Array(const REBVAL *tset)
{
    REBARR *block;
    REBVAL *value;
    REBINT n;
    REBINT size = 0;

    for (n = 0; n < REB_MAX; n++) {
        if (TYPE_CHECK(tset, cast(enum Reb_Kind, n))) size++;
    }

    block = Make_Array(size);

    // Convert bits to types:
    for (n = 0; n < REB_MAX; n++) {
        if (TYPE_CHECK(tset, cast(enum Reb_Kind, n))) {
            value = Alloc_Tail_Array(block);
            if (n == 0) {
                //
                // !!! A NONE! value is currently supported in typesets to
                // indicate that they take optional values.  This may wind up
                // as a feature of MAKE FUNCTION! only.
                //
                SET_BLANK(value);
            }
            else
                Val_Init_Datatype(value, cast(enum Reb_Kind, n));
        }
    }
    return block;
}


//
//  REBTYPE: C
//
REBTYPE(Typeset)
{
    REBVAL *val = D_ARG(1);
    REBVAL *arg = D_ARGC > 1 ? D_ARG(2) : NULL;

    switch (action) {

    case SYM_FIND:
        if (IS_DATATYPE(arg)) {
            return (TYPE_CHECK(val, VAL_TYPE_KIND(arg))) ? R_TRUE : R_FALSE;
        }
        fail (Error_Invalid_Arg(arg));

    case SYM_AND_T:
    case SYM_OR_T:
    case SYM_XOR_T:
        if (IS_DATATYPE(arg)) {
            VAL_TYPESET_BITS(arg) = FLAGIT_KIND(VAL_TYPE(arg));
        }
        else if (!IS_TYPESET(arg))
            fail (Error_Invalid_Arg(arg));

        if (action == SYM_OR_T)
            VAL_TYPESET_BITS(val) |= VAL_TYPESET_BITS(arg);
        else if (action == SYM_AND_T)
            VAL_TYPESET_BITS(val) &= VAL_TYPESET_BITS(arg);
        else
            VAL_TYPESET_BITS(val) ^= VAL_TYPESET_BITS(arg);
        *D_OUT = *D_ARG(1);
        return R_OUT;

    case SYM_COMPLEMENT:
        VAL_TYPESET_BITS(val) = ~VAL_TYPESET_BITS(val);
        *D_OUT = *D_ARG(1);
        return R_OUT;

    default:
        fail (Error_Illegal_Action(REB_TYPESET, action));
    }
}
