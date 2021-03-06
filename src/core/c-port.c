//
//  File: %c-port.c
//  Summary: "support for I/O ports"
//  Section: core
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
// See comments in Init_Ports for startup.
// See www.rebol.net/wiki/Event_System for full details.
//

#include "sys-core.h"

#define MAX_WAIT_MS 64 // Maximum millsec to sleep


//
//  Is_Port_Open: C
//
// Standard method for checking if port is open.
// A convention. Not all ports use this method.
//
REBOOL Is_Port_Open(REBCTX *port)
{
    REBVAL *state = CTX_VAR(port, STD_PORT_STATE);
    if (!IS_BINARY(state)) return FALSE;
    return IS_OPEN(VAL_BIN_AT(state));
}


//
//  Set_Port_Open: C
//
// Standard method for setting a port open/closed.
// A convention. Not all ports use this method.
//
void Set_Port_Open(REBCTX *port, REBOOL open)
{
    REBVAL *state = CTX_VAR(port, STD_PORT_STATE);
    if (IS_BINARY(state)) {
        if (open) SET_OPEN(VAL_BIN_AT(state));
        else SET_CLOSED(VAL_BIN_AT(state));
    }
}


//
//  Use_Port_State: C
//
// Use private state area in a port. Create if necessary.
// The size is that of a binary structure used by
// the port for storing internal information.
//
void *Use_Port_State(REBCTX *port, REBCNT device, REBCNT size)
{
    REBVAL *state = CTX_VAR(port, STD_PORT_STATE);

    // If state is not a binary structure, create it:
    if (!IS_BINARY(state)) {
        REBSER *data = Make_Binary(size);
        REBREQ *req = cast(REBREQ*, BIN_HEAD(data));
        req->clen = size;
        CLEAR(BIN_HEAD(data), size);
        //data->tail = size; // makes it easier for ACCEPT to clone the port
        SET_FLAG(req->flags, RRF_ALLOC); // not on stack
        req->port = port;
        req->device = device;
        Init_Binary(state, data);
    }

    return (void *)VAL_BIN(state);
}


//
//  Pending_Port: C
//
// Return TRUE if port value is pending a signal.
// Not valid for all ports - requires request struct!!!
//
REBOOL Pending_Port(REBVAL *port)
{
    REBVAL *state;
    REBREQ *req;

    if (IS_PORT(port)) {
        state = CTX_VAR(VAL_CONTEXT(port), STD_PORT_STATE);
        if (IS_BINARY(state)) {
            req = (REBREQ*)VAL_BIN(state);
            if (!GET_FLAG(req->flags, RRF_PENDING)) return FALSE;
        }
    }
    return TRUE;
}


//
//  Awake_System: C
//
// Returns:
//     -1 for errors
//      0 for nothing to do
//      1 for wait is satisifed
//
REBINT Awake_System(REBARR *ports, REBOOL only)
{
    REBVAL *port;
    REBVAL *state;
    REBVAL *waked;
    REBVAL *awake;
    REBVAL awake_only;

    REBVAL tmp;
    REBVAL result;

    // Get the system port object:
    port = Get_System(SYS_PORTS, PORTS_SYSTEM);
    if (!IS_PORT(port)) return -10; // verify it is a port object

    // Get wait queue block (the state field):
    state = VAL_CONTEXT_VAR(port, STD_PORT_STATE);
    if (!IS_BLOCK(state)) return -10;
    //Debug_Num("S", VAL_LEN_HEAD(state));

    // Get waked queue block:
    waked = VAL_CONTEXT_VAR(port, STD_PORT_DATA);
    if (!IS_BLOCK(waked)) return -10;

    // If there is nothing new to do, return now:
    if (VAL_LEN_HEAD(state) == 0 && VAL_LEN_HEAD(waked) == 0) return -1;

    //Debug_Num("A", VAL_LEN_HEAD(waked));
    // Get the system port AWAKE function:
    awake = VAL_CONTEXT_VAR(port, STD_PORT_AWAKE);
    if (!IS_FUNCTION(awake)) return -1;

    if (ports)
        Init_Block(&tmp, ports);
    else
        SET_BLANK(&tmp);

    if (only) {
        //
        // If we're using /ONLY, we need path AWAKE/ONLY to call.  (Ren-C's
        // va_list API does not support positionally-provided refinements.)
        //
        REBARR *array = Make_Array(2);
        Append_Value(array, awake);
        Init_Word(Alloc_Tail_Array(array), Canon(SYM_ONLY));

        Init_Path(&awake_only, array);
    }

    // Call the system awake function:
    //
    if (Apply_Only_Throws(
        &result,
        TRUE,
        only ? &awake_only : awake,
        port,
        &tmp,
        END_CELL
    )) {
        fail (Error_No_Catch_For_Throw(&result));
    }

    // Awake function returns 1 for end of WAIT:
    //
    return (IS_LOGIC(&result) && VAL_LOGIC(&result)) ? 1 : 0;
}


//
//  Wait_Ports: C
//
// Inputs:
//     Ports: a block of ports or zero (on stack to avoid GC).
//     Timeout: milliseconds to wait
//
// Returns:
//     TRUE when port action happened, or FALSE for timeout.
//
REBOOL Wait_Ports(REBARR *ports, REBCNT timeout, REBOOL only)
{
    REBI64 base = OS_DELTA_TIME(0, 0);
    REBCNT time;
    REBINT result;
    REBCNT wt = 1;
    REBCNT res = (timeout >= 1000) ? 0 : 16;  // OS dependent?

    // Waiting opens the doors to pressing Ctrl-C, which may get this code
    // to throw an error.  There needs to be a state to catch it.
    //
    assert(Saved_State != NULL);

    while (wt) {
        if (GET_SIGNAL(SIG_HALT)) {
            CLR_SIGNAL(SIG_HALT);
            fail (VAL_CONTEXT(TASK_HALT_ERROR));
        }

        if (GET_SIGNAL(SIG_INTERRUPT)) {
            CLR_SIGNAL(SIG_INTERRUPT);

            REBVAL result;
            if (Do_Breakpoint_Throws(&result, TRUE, VOID_CELL, FALSE)) {
                //
                // !!! Consider Wait_Ports() callsites being re-engineered
                // to be able to gracefully accept a throw generated by
                // a RESUME from a breakpoint, e.g. `resume/do [throw 10]`.
                // This would require having a return result.
                //
                fail (Error_No_Catch_For_Throw(&result));
            }
            if (!IS_VOID(&result)) {
                //
                // !!! Same as above... if `resume/with 10` is to have any
                // meaning then there must be a way to deliver that result
                // up the stack.
                //
                fail (Error(RE_MISC));
            }
        }

        // Process any waiting events:
        if ((result = Awake_System(ports, only)) > 0) return TRUE;

        // If activity, use low wait time, otherwise increase it:
        if (result == 0) wt = 1;
        else {
            wt *= 2;
            if (wt > MAX_WAIT_MS) wt = MAX_WAIT_MS;
        }

        if (timeout != ALL_BITS) {
            // Figure out how long that (and OS_WAIT) took:
            time = (REBCNT)(OS_DELTA_TIME(base, 0)/1000);
            if (time >= timeout) break;   // done (was dt = 0 before)
            else if (wt > timeout - time) // use smaller residual time
                wt = timeout - time;
        }

        //printf("%d %d %d\n", dt, time, timeout);

        // Wait for events or time to expire:
        //Debug_Num("OSW", wt);
        OS_WAIT(wt, res);
    }

    //time = (REBCNT)OS_DELTA_TIME(base, 0);
    //Print("dt: %d", time);

    return FALSE; // timeout
}


//
//  Sieve_Ports: C
//
// Remove all ports not found in the WAKE list.
// ports could be NULL, in which case the WAKE list is cleared.
//
void Sieve_Ports(REBARR *ports)
{
    REBVAL *port;
    REBVAL *waked;
    REBCNT n;

    port = Get_System(SYS_PORTS, PORTS_SYSTEM);
    if (!IS_PORT(port)) return;
    waked = VAL_CONTEXT_VAR(port, STD_PORT_DATA);
    if (!IS_BLOCK(waked)) return;

    for (n = 0; ports && n < ARR_LEN(ports);) {
        RELVAL *val = ARR_AT(ports, n);
        if (IS_PORT(val)) {
            assert(VAL_LEN_HEAD(waked) != 0);
            if (
                Find_In_Array_Simple(VAL_ARRAY(waked), 0, val)
                == VAL_LEN_HEAD(waked) // `=len` means not found
            ) {
                Remove_Series(AS_SERIES(ports), n, 1);
                continue;
            }
        }
        n++;
    }
    //clear waked list
    RESET_ARRAY(VAL_ARRAY(waked));
}


//
//  Find_Action: C
//
// Given an action number, return the action's index in
// the specified object. If not found, a zero is returned.
//
REBCNT Find_Action(REBVAL *object, REBSYM action)
{
    return Find_Canon_In_Context(VAL_CONTEXT(object), Canon(action), FALSE);
}


//
//  Redo_Func_Throws: C
//
// This code takes a running call frame that has been built for one function
// and then tries to map its parameters to another call.  It is used to
// dispatch some ACTION!s (an archetypal function spec with no implementation)
// from a native C invocation to be "bounced" out into user code.
//
// In the origins of this function's active usage in R3-Alpha, it was allowed
// for the target function to have a parameterization that was a superset of
// the original frame's function (adding refinements, etc.)  The greater
// intentions of how it was supposed to work are not known--as there was
// little error checking, given there were few instances.
//
// !!! Due to the historical brittleness of this function, very rare calls,
// and need for an additional repetition of dispatch logic from Do_Core,
// this code has been replaced with a straightforward implementation.  It
// builds a PATH! of the target function and refinements from the original
// frame.  Then it uses this in the DO_FLAG_EVAL_ONLY mode to suppress
// re-evaluation of the frame's "live" args.
//
// !!! This won't stand up in the face of targets that are "adversarial"
// to the archetype:
//
//     foo: func [a /b c] [...]  =>  bar: func [/b d e] [...]
//                    foo/b 1 2  =>  bar/b 1 2
//
// However, it is still *much* better than the R3-Alpha situation for error
// checking, and significantly less confusing.  A real solution to this kind
// of dispatch--if it is to be used--seems like it should be a language
// feature available to users themselves.  So leaning on the evaluator in
// one way or another is the best course to keep this functionality going.
//
REBOOL Redo_Func_Throws(REBFRM *f, REBFUN *func_new)
{
    // Upper bound on the length of the args we might need for a redo
    // invocation is the total number of parameters to the *old* function's
    // invocation (if it had no refinements or locals).
    //
    REBARR *code_array = Make_Array(FUNC_NUM_PARAMS(f->func));
    RELVAL *code = ARR_HEAD(code_array);

    // We'll walk through the original functions param and arglist only, and
    // accept the error-checking the evaluator provides at this time (types,
    // refinement presence or absence matching).
    //
    // !!! See note in function description about arity mismatches.
    //
    f->param = FUNC_PARAMS_HEAD(f->underlying);
    f->arg = f->args_head;
    REBOOL ignoring = FALSE;

    // The first element of our path will be the function, followed by its
    // refinements.  It has an upper bound on length that is to consider the
    // opposite case where it had only refinements and then the function
    // at the head...
    //
    REBARR *path_array = Make_Array(FUNC_NUM_PARAMS(f->func) + 1);
    RELVAL *path = ARR_HEAD(path_array);

    REBVAL first;

    *path = *FUNC_VALUE(func_new);
    ++path;

    for (; NOT_END(f->param); ++f->param, ++f->arg) {
        enum Reb_Param_Class pclass = VAL_PARAM_CLASS(f->param);

        if (
            pclass == PARAM_CLASS_LOCAL
            || pclass == PARAM_CLASS_LEAVE
            || pclass == PARAM_CLASS_RETURN
        ) {
             continue; // don't add a callsite expression for it (can't)!
        }

        if (pclass == PARAM_CLASS_REFINEMENT) {
            if (IS_CONDITIONAL_FALSE(f->arg)) {
                //
                // If the refinement is not in use, do not add it and ignore
                // args until the next refinement.
                //
                ignoring = TRUE;
                continue;
            }

            // In use--and used refinements must be added to the PATH!
            //
            ignoring = FALSE;
            Init_Word(path, VAL_PARAM_SPELLING(f->param));
            ++path;
            continue;
        }

        // Otherwise it should be a quoted or normal argument.  If ignoring
        // then pass on it, otherwise add the arg to the code as-is.
        //
        if (ignoring) continue;

        *code = *f->arg;
        ++code;
    }

    TERM_ARRAY_LEN(code_array, code - ARR_HEAD(code_array));
    MANAGE_ARRAY(code_array);

    TERM_ARRAY_LEN(path_array, path - ARR_HEAD(path_array));
    Init_Path(&first, path_array);

    // Invoke DO with the special mode requesting non-evaluation on all
    // args, as they were evaluated the first time around.
    //
    REBIXO indexor = Do_Array_At_Core(
        f->out,
        &first, // path not in array, will be "virtual" first element
        code_array,
        0, // index
        SPECIFIED, // reusing existing REBVAL arguments, no relative values
        DO_FLAG_NO_ARGS_EVALUATE
    );

    if (indexor != THROWN_FLAG && indexor != END_FLAG) {
        //
        // We may not have stopped the invocation by virtue of the args
        // all not getting consumed, but we can raise an error now that it
        // did not.
        //
        assert(FALSE);
        fail (Error(RE_MISC));
    }

    return LOGICAL(indexor == THROWN_FLAG);
}


//
//  Do_Port_Action: C
//
// Call a PORT actor (action) value. Search PORT actor
// first. If not found, search the PORT scheme actor.
//
// NOTE: stack must already be setup correctly for action, and
// the caller must cleanup the stack.
//
REB_R Do_Port_Action(REBFRM *frame_, REBCTX *port, REBSYM action)
{
    assert(GET_SER_FLAG(CTX_VARLIST(port), ARRAY_FLAG_VARLIST));

    // Verify valid port (all of these must be false):
    if (
        // Must be = or larger than std port:
        (CTX_LEN(port) < STD_PORT_MAX - 1) ||
        // Must have a spec object:
        !IS_OBJECT(CTX_VAR(port, STD_PORT_SPEC))
    ) {
        fail (Error(RE_INVALID_PORT));
    }

    // Get actor for port, if it has one:

    REBVAL *actor = CTX_VAR(port, STD_PORT_ACTOR);

    if (IS_BLANK(actor))
        return R_BLANK;

    REB_R r;
    // If actor is a function (!!! Note: must be native !!!)
    if (IS_FUNCTION(actor)) {
        r = cast(REBPAF, VAL_FUNC_DISPATCHER(actor))(frame_, port, action);
        goto post_process_output;
    }

    // actor must be an object:
    if (!IS_OBJECT(actor))
        fail (Error(RE_INVALID_ACTOR));

    // Dispatch object function:

    REBCNT n; // goto would cross initialization
    n = Find_Action(actor, action);
    actor = Obj_Value(actor, n);
    if (!n || !actor || !IS_FUNCTION(actor)) {
        REBVAL action_word;
        Init_Word(&action_word, Canon(action));

        fail (Error(RE_NO_PORT_ACTION, &action_word));
    }

    if (Redo_Func_Throws(frame_, VAL_FUNC(actor))) {
        // The throw name will be in D_OUT, with thrown value in task vars
        return R_OUT_IS_THROWN;
    }

    r = R_OUT; // result should be in frame_->out

    // !!! READ's /LINES and /STRING refinements are something that should
    // work regardless of data source.  But R3-Alpha only implemented it in
    // %p-file.c, so it got ignored.  Ren-C caught that it was being ignored,
    // so the code was moved to here as a quick fix.
    //
    // !!! Note this code is incorrect for files read in chunks!!!

post_process_output:
    if (action == SYM_READ) {
        INCLUDE_PARAMS_OF_READ;

        UNUSED(PAR(source));
        UNUSED(PAR(part));
        UNUSED(PAR(limit));
        UNUSED(PAR(seek));
        UNUSED(PAR(index));

        assert(r == R_OUT);

        if ((REF(string) || REF(lines)) && !IS_STRING(D_OUT)) {
            if (!IS_BINARY(D_OUT))
                fail (Error(RE_MISC)); // !!! when can this happen?

            REBSER *decoded = Decode_UTF_String(
                VAL_BIN_AT(D_OUT),
                VAL_LEN_AT(D_OUT),
                -1
            );
            if (decoded == NULL)
                fail (Error(RE_BAD_UTF8));
            Init_String(D_OUT, decoded);
        }

        if (REF(lines)) { // caller wants a BLOCK! of STRING!s, not one string
            if (!IS_STRING(D_OUT))
                fail (Error(RE_MISC)); // !!! when can this happen?

            REBVAL temp = *D_OUT;
            Init_Block(D_OUT, Split_Lines(&temp));
        }
    }

    return r;
}


//
//  Secure_Port: C
//
// kind: word that represents the type (e.g. 'file)
// req:  I/O request
// name: value that holds the original user spec
// path: the local path to compare with
//
void Secure_Port(REBSYM sym_kind, REBREQ *req, REBVAL *name, REBSER *path)
{
    REBVAL val;
    Init_String(&val, path);

    REBYTE *flags = Security_Policy(Canon(sym_kind), &val); // policy flags

    // Check policy integer:
    // Mask is [xxxx wwww rrrr] - each holds the action
    if (GET_FLAG(req->modes, RFM_READ))
        Trap_Security(flags[POL_READ], Canon(sym_kind), name);

    if (GET_FLAG(req->modes, RFM_WRITE))
        Trap_Security(flags[POL_WRITE], Canon(sym_kind), name);
}


#ifdef HAS_POSIX_SIGNAL
#define MAX_SCHEMES 12      // max native schemes
#else
#define MAX_SCHEMES 11      // max native schemes
#endif

typedef struct rebol_scheme_actions {
    REBSTR *name;
    REBPAF fun;
} SCHEME_ACTIONS;

SCHEME_ACTIONS *Scheme_Actions; // Initial Global (not threaded)


//
//  Register_Scheme: C
//
// Associate a scheme word (e.g. FILE) with a set of native
// scheme actions. This will be used by the Set_Scheme native
//
void Register_Scheme(REBSTR *name, REBPAF fun)
{
    REBINT n;

    for (n = 0; n < MAX_SCHEMES && Scheme_Actions[n].name != NULL; n++);
    assert(n < MAX_SCHEMES);

    Scheme_Actions[n].name = name;
    Scheme_Actions[n].fun = fun;
}


//
//  set-scheme: native [
//
//  "Low-level port scheme actor initialization."
//
//      scheme [object!]
//  ]
//
REBNATIVE(set_scheme)
{
    INCLUDE_PARAMS_OF_SET_SCHEME;

    REBVAL *name = Obj_Value(ARG(scheme), STD_SCHEME_NAME);
    if (!IS_WORD(name))
        fail (Error(RE_NO_SCHEME_NAME));

    REBVAL *actor = Obj_Value(ARG(scheme), STD_SCHEME_ACTOR);
    if (!actor) return R_BLANK;

    REBCNT n;
    // Does this scheme have native actor or actions?
    for (n = 0; n < MAX_SCHEMES && Scheme_Actions[n].name != NULL; n++) {
        if (SAME_STR(Scheme_Actions[n].name, VAL_WORD_SPELLING(name))) break;
    }

    if (n == MAX_SCHEMES || Scheme_Actions[n].name == NULL)
        return R_BLANK;

    // !!! A previously unused aspect of registering "scheme handlers" was the
    // idea that instead of a native PAF function ("port action function"),
    // some kind of map was provided.  Ren-C is going in a different direction
    // with this, and since it wasn't used that was deleted.  The only scheme
    // handler supported is a REBPAF "native actor"
    //
    assert(Scheme_Actions[n].fun);

    REBVAL *port_actor_spec = Get_System(SYS_STANDARD, STD_PORT_ACTOR_SPEC);

    // !!! Not sure what this code was supposed to do, and it is
    // deprecated.  It takes a single argument, and currently the spec
    // allows [<opt> any-value!].

    REBFUN *fun = Make_Function(
        Make_Paramlist_Managed_May_Fail(port_actor_spec, MKF_KEYWORDS),
        cast(REBNAT, Scheme_Actions[n].fun), // !!! actually a REBPAF (!!!)
        NULL // no underlying function, fundamental
    );

    *actor = *FUNC_VALUE(fun);

    return R_TRUE;
}


//
//  Init_Ports: C
//
// Initialize port scheme related subsystems.
//
// In order to add a port scheme:
//
// In mezz-ports.r add a make-scheme.
// Add an Init_*_Scheme() here.
// Be sure host-devices.c has the device enabled.
//
void Init_Ports(void)
{
    Scheme_Actions = ALLOC_N(SCHEME_ACTIONS, MAX_SCHEMES);
    CLEAR(Scheme_Actions, MAX_SCHEMES * sizeof(SCHEME_ACTIONS));

    Init_Console_Scheme();
    Init_File_Scheme();
    Init_Dir_Scheme();
    Init_Event_Scheme();
    Init_TCP_Scheme();
    Init_UDP_Scheme();
    Init_DNS_Scheme();

#ifdef TO_WINDOWS
    Init_Clipboard_Scheme();
#endif

#if defined(TO_LINUX) || defined(TO_WINDOWS)
    Init_Serial_Scheme();
#endif

#ifdef HAS_POSIX_SIGNAL
    Init_Signal_Scheme();
#endif
}


//
//  Shutdown_Ports: C
//
void Shutdown_Ports(void)
{
    FREE_N(SCHEME_ACTIONS, MAX_SCHEMES, Scheme_Actions);
}
