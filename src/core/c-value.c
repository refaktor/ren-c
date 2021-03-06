//
//  File: %c-value.c
//  Summary: "Generic REBVAL Support Services and Debug Routines"
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2016 Rebol Open Source Contributors
// REBOL is a trademark of REBOL Technologies
//
// See README.md and CREDITS.md for more information
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
// These are mostly DEBUG-build routines to support the macros and definitions
// in %sys-value.h.
//
// These are not specific to any given type.  For the type-specific REBVAL
// code, see files with names like %t-word.c, %t-logic.c, %t-integer.c...
//

#include "sys-core.h"


#if !defined(NDEBUG)

//
//  Panic_Value_Debug: C
//
// This is a debug-only "error generator", which will hunt through all the
// series allocations and panic on the series that contains the value (if
// it can find it).  This will allow those using Address Sanitizer or
// Valgrind to know a bit more about where the value came from.
//
// Additionally, if it happens to be a void or trash, LOGIC!, BAR!, or NONE!
// it will dump out where the initialization happened if that information
// was stored.
//
ATTRIBUTE_NO_RETURN void Panic_Value_Debug(const RELVAL *v) {
    fflush(stdout);
    fflush(stderr);

    REBSER *containing = Try_Find_Containing_Series_Debug(v);

    switch (VAL_TYPE_RAW(v)) {
    case REB_MAX_VOID:
    case REB_BLANK:
    case REB_LOGIC:
    case REB_BAR:
        printf(
            "REBVAL init on tick #%d at %s:%d\n",
            cast(unsigned int, v->extra.do_count),
            v->payload.track.filename,
            v->payload.track.line
        );
        fflush(stdout);
        break;

    default:
        break;
    }

    printf("Kind=%d\n", cast(int, VAL_TYPE_RAW(v)));
    fflush(stdout);

    if (containing != NULL) {
        printf("Containing series for value pointer found, panicking it:\n");
        Panic_Series_Debug(containing);
    }

    printf("No containing series for value...panicking to make stack dump:\n");
    Panic_Series_Debug(AS_SERIES(EMPTY_ARRAY));
}


#if defined(__cplusplus)

//
//  Assert_Cell_Writable: C
//
// The check helps avoid very bad catastrophies that might ensue if "implicit
// end markers" could be overwritten.  These are the ENDs that are actually
// pointers doing double duty inside a data structure, and there is no REBVAL
// storage backing the position.
//
// (A fringe benefit is catching writes to other unanticipated locations.)
//
void Assert_Cell_Writable(const RELVAL *v, const char *file, int line)
{
    // REBVALs should not be written at addresses that do not match the
    // alignment of the processor.  This checks modulo the size of an unsigned
    // integer the same size as a platform pointer (REBUPT => uintptr_t)
    //
    assert(cast(REBUPT, (v)) % sizeof(REBUPT) == 0);

    if (NOT((v)->header.bits & NODE_FLAG_CELL)) {
        printf("Non-cell passed to writing routine\n");
        panic_at (v, file, line);
    }
    if (NOT((v)->header.bits & NODE_FLAG_VALID)) {
        printf("Non-writable value passed to writing routine\n");
        panic_at (v, file, line);
    }
}

#endif


//
//  SET_END_Debug: C
//
// Uses REB_0 for the type, to help cue debugging.
//
// When SET_END is used, it uses the whole cell.  Implicit termination is
// done by the raw creation of a Reb_Header in the containing structure.
//
void SET_END_Debug(RELVAL *v, const char *file, int line) {
    ASSERT_CELL_WRITABLE_IF_CPP_DEBUG(v, file, line);
    (v)->header.bits = HEADERIZE_KIND(REB_0) | FLAGBYTE_FIRST(255);
    Set_Track_Payload_Debug(v, file, line);
}


//
//  IS_END_Debug: C
//
REBOOL IS_END_Debug(const RELVAL *v, const char *file, int line) {
#ifdef __cplusplus
    if (
        (v->header.bits & NODE_FLAG_CELL)
        //
        // Note: a non-writable value could have any bit pattern in the
        // type slot, so we only check for trash in writable ones.
        //
        && VAL_TYPE_RAW(v) == REB_MAX_VOID
        && NOT(v->header.bits & VOID_FLAG_NOT_TRASH)
        && NOT(v->header.bits & VOID_FLAG_SAFE_TRASH)
    ) {
        printf("IS_END() called on value marked as TRASH\n");
        panic_at (v, file, line);
    }
#endif

    if (IS_END_MACRO(v)) {
        if (v->header.bits & NODE_FLAG_CELL)
            assert(LEFT_N_BITS(v->header.bits, 8) == 255);
        return TRUE;
    }
    return FALSE;
}


//
//  VAL_SPECIFIC_Debug: C
//
REBCTX *VAL_SPECIFIC_Debug(const REBVAL *v)
{
    assert(NOT_VAL_FLAG(v, VALUE_FLAG_RELATIVE));
    assert(
        ANY_WORD(v)
        || ANY_ARRAY(v)
        || IS_VARARGS(v)
        || IS_FUNCTION(v)
        || ANY_CONTEXT(v)
    );

    REBCTX *specific = VAL_SPECIFIC_COMMON(v);

    if (specific != SPECIFIED) {
        //
        // Basic sanity check: make sure it's a context at all
        //
        if (NOT_SER_FLAG(CTX_VARLIST(specific), ARRAY_FLAG_VARLIST)) {
            printf("Non-CONTEXT found as specifier in specific value\n");
            panic (specific); // may not be a series, either
        }

        // While an ANY-WORD! can be bound specifically to an arbitrary
        // object, an ANY-ARRAY! only becomes bound specifically to frames.
        // The keylist for a frame's context should come from a function's
        // paramlist, which should have a FUNCTION! value in keylist[0]
        //
        if (ANY_ARRAY(v))
            assert(IS_FUNCTION(CTX_ROOTKEY(specific)));
    }

    return specific;
}


//
//  Assert_No_Relative: C
//
// Check to make sure there are no relative values in an array, maybe deeply.
//
// !!! What if you have an ANY-ARRAY! inside your array at a position N,
// but there is a relative value in the VAL_ARRAY() of that value at an
// index earlier than N?  This currently considers that an error since it
// checks the whole array...which is more conservative (asserts on more
// cases).  But should there be a flag to ask to honor the index?
//
void Assert_No_Relative(REBARR *array, REBOOL deep)
{
    RELVAL *item = ARR_HEAD(array);
    while (NOT_END(item)) {
        if (IS_RELATIVE(item)) {
            printf("Array contained relative item and wasn't supposed to\n");
            panic (item);
        }
        if (!IS_UNREADABLE_IF_DEBUG(item) && ANY_ARRAY(item) && deep)
             Assert_No_Relative(VAL_ARRAY(item), deep);
        ++item;
    }
}


//
//  Probe_Core_Debug: C
//
void Probe_Core_Debug(
    const void *p,
    const char *file,
    int line
) {
    const struct Reb_Header *h = cast(const struct Reb_Header*, p);

    printf("\n** PROBE() ");
    printf("tick %d %s:%d\n", cast(int, TG_Do_Count), file, line);

    fflush(stdout);
    fflush(stderr);

    if (h->bits & NODE_FLAG_CELL)
        Debug_Fmt("%r\n", cast(const REBVAL*, p));
    else {
        REBSER *s = m_cast(REBSER*, cast(const REBSER*, p));
        if (GET_SER_FLAG(s, ARRAY_FLAG_VARLIST)) {
            REBCTX *c = AS_CONTEXT(s);

            // Don't use Init_Any_Context, because that can implicitly manage
            // the context...which we don't want a debug dump routine to do.
            //
            REBVAL temp;
            VAL_RESET_HEADER(&temp, CTX_TYPE(c));
            temp.extra.binding = NULL;
            temp.payload.any_context.varlist = CTX_VARLIST(c);
        }
        else
            Debug_Series(s); // handles arrays
    }
}
#endif
