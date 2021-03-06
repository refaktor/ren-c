//
//  File: %p-net.c
//  Summary: "network port interface"
//  Section: ports
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

#include "reb-net.h"
#include "reb-evtypes.h"

#define NET_BUF_SIZE 32*1024

enum Transport_Types {
    TRANSPORT_TCP,
    TRANSPORT_UDP
};

//
//  Ret_Query_Net: C
//
static void Ret_Query_Net(REBCTX *port, REBREQ *sock, REBVAL *out)
{
    REBVAL *std_info = In_Object(port, STD_PORT_SCHEME, STD_SCHEME_INFO, 0);
    REBCTX *info;

    if (!std_info || !IS_OBJECT(std_info))
        fail (Error_On_Port(RE_INVALID_SPEC, port, -10));

    info = Copy_Context_Shallow(VAL_CONTEXT(std_info));

    Set_Tuple(
        CTX_VAR(info, STD_NET_INFO_LOCAL_IP),
        cast(REBYTE*, &sock->special.net.local_ip),
        4
    );
    SET_INTEGER(
        CTX_VAR(info, STD_NET_INFO_LOCAL_PORT),
        sock->special.net.local_port
    );

    Set_Tuple(
        CTX_VAR(info, STD_NET_INFO_REMOTE_IP),
        cast(REBYTE*, &sock->special.net.remote_ip),
        4
    );
    SET_INTEGER(
        CTX_VAR(info, STD_NET_INFO_REMOTE_PORT),
        sock->special.net.remote_port
    );

    Init_Object(out, info);
}


//
//  Accept_New_Port: C
//
// Clone a listening port as a new accept port.
//
static void Accept_New_Port(REBVAL *out, REBCTX *port, REBREQ *sock)
{
    REBREQ *nsock;

    // Get temp sock struct created by the device:
    nsock = sock->common.sock;
    if (!nsock) return;  // false alarm
    sock->common.sock = nsock->next;
    nsock->common.data = 0;
    nsock->next = 0;

    // Create a new port using ACCEPT request passed by sock->common.sock:
    port = Copy_Context_Shallow(port);
    Init_Port(out, port); // Also for GC protect

    SET_BLANK(CTX_VAR(port, STD_PORT_DATA)); // just to be sure.
    SET_BLANK(CTX_VAR(port, STD_PORT_STATE)); // just to be sure.

    // Copy over the new sock data:
    sock = cast(REBREQ*, Use_Port_State(port, RDI_NET, sizeof(*sock)));
    *sock = *nsock;
    sock->clen = sizeof(*sock);
    sock->port = port;
    OS_FREE(nsock); // allocated by dev_net.c (MT issues?)
}

//
//  Transport_Actor: C
//
static REB_R Transport_Actor(
    REBFRM *frame_,
    REBCTX *port,
    REBSYM action,
    enum Transport_Types proto
) {
    // Initialize the IO request
    //
    REBREQ *sock = cast(REBREQ*, Use_Port_State(port, RDI_NET, sizeof(*sock)));
    if (proto == TRANSPORT_UDP)
        SET_FLAG(sock->modes, RST_UDP);

    REBVAL *spec = CTX_VAR(port, STD_PORT_SPEC);
    if (!IS_OBJECT(spec))
        fail (Error(RE_INVALID_PORT));

    // sock->timeout = 4000; // where does this go? !!!

    // !!! Comment said "HOW TO PREVENT OVERWRITE DURING BUSY OPERATION!!!
    // Should it just ignore it or cause an error?"

    // Actions for an unopened socket:

    if (!IS_OPEN(sock)) {

        switch (action) { // Ordered by frequency

        case SYM_OPEN: {
            REBVAL *arg = Obj_Value(spec, STD_PORT_SPEC_NET_HOST);
            REBVAL *val = Obj_Value(spec, STD_PORT_SPEC_NET_PORT_ID);

            if (OS_DO_DEVICE(sock, RDC_OPEN))
                fail (Error_On_Port(RE_CANNOT_OPEN, port, -12));
            SET_OPEN(sock);

            // Lookup host name (an extra TCP device step):
            if (IS_STRING(arg)) {
                sock->common.data = VAL_BIN(arg);
                sock->special.net.remote_port =
                    IS_INTEGER(val) ? VAL_INT32(val) : 80;

                // Note: sets remote_ip field
                //
                REBINT result = OS_DO_DEVICE(sock, RDC_LOOKUP);
                if (result < 0)
                    fail (Error_On_Port(RE_NO_CONNECT, port, sock->error));

                *D_OUT = *CTX_VALUE(port);
                return R_OUT;
            }
            else if (IS_TUPLE(arg)) { // Host IP specified:
                sock->special.net.remote_port =
                    IS_INTEGER(val) ? VAL_INT32(val) : 80;
                memcpy(&sock->special.net.remote_ip, VAL_TUPLE(arg), 4);
                break;
            }
            else if (IS_BLANK(arg)) { // No host, must be a LISTEN socket:
                SET_FLAG(sock->modes, RST_LISTEN);
                sock->common.data = 0; // where ACCEPT requests are queued
                sock->special.net.local_port =
                    IS_INTEGER(val) ? VAL_INT32(val) : 8000;
                break;
            }
            else
                fail (Error_On_Port(RE_INVALID_SPEC, port, -10));
            break; }

        case SYM_CLOSE:
            *D_OUT = *CTX_VALUE(port);
            return R_OUT;

        case SYM_OPEN_Q:
            return R_FALSE;

        case SYM_UPDATE:  // allowed after a close
            break;

        default:
            fail (Error_On_Port(RE_NOT_OPEN, port, -12));
        }
    }

    // Actions for an open socket:

    switch (action) { // Ordered by frequency

    case SYM_UPDATE: {
        //
        // Update the port object after a READ or WRITE operation.
        // This is normally called by the WAKE-UP function.
        //
        REBVAL *port_data = CTX_VAR(port, STD_PORT_DATA);
        if (sock->command == RDC_READ) {
            if (ANY_BINSTR(port_data)) {
                SET_SERIES_LEN(
                    VAL_SERIES(port_data),
                    VAL_LEN_HEAD(port_data) + sock->actual
                );
            }
        }
        else if (sock->command == RDC_WRITE) {
            SET_BLANK(port_data); // Write is done.
        }
        return R_BLANK; }

    case SYM_READ: {
        INCLUDE_PARAMS_OF_READ;

        UNUSED(PAR(source));

        if (REF(part)) {
            assert(!IS_VOID(ARG(limit)));
            fail (Error(RE_BAD_REFINES));
        }
        if (REF(seek)) {
            assert(!IS_VOID(ARG(index)));
            fail (Error(RE_BAD_REFINES));
        }
        UNUSED(PAR(string)); // handled in dispatcher
        UNUSED(PAR(lines)); // handled in dispatcher

        // Read data into a buffer, expanding the buffer if needed.
        // If no length is given, program must stop it at some point.
        if (
            !GET_FLAG(sock->modes, RST_UDP)
            && !GET_FLAG(sock->state, RSM_CONNECT)
        ) {
            fail (Error_On_Port(RE_NOT_CONNECTED, port, -15));
        }

        // Setup the read buffer (allocate a buffer if needed):
        //
        REBVAL *port_data = CTX_VAR(port, STD_PORT_DATA);
        REBSER *buffer;
        if (!IS_STRING(port_data) && !IS_BINARY(port_data)) {
            buffer = Make_Binary(NET_BUF_SIZE);
            Init_Binary(port_data, buffer);
        }
        else {
            buffer = VAL_SERIES(port_data);
            assert(BYTE_SIZE(buffer));

            if (SER_AVAIL(buffer) < NET_BUF_SIZE/2)
                Extend_Series(buffer, NET_BUF_SIZE);
        }

        sock->length = SER_AVAIL(buffer);
        sock->common.data = BIN_TAIL(buffer); // write at tail
        sock->actual = 0; // actual for THIS read (not for total)

        // Note: recv can happen immediately
        //
        REBINT result = OS_DO_DEVICE(sock, RDC_READ);
        if (result < 0)
            fail (Error_On_Port(RE_READ_ERROR, port, sock->error));

        *D_OUT = *CTX_VALUE(port);
        return R_OUT; }

    case SYM_WRITE: {
        INCLUDE_PARAMS_OF_WRITE;

        UNUSED(PAR(destination));

        if (REF(seek)) {
            assert(!IS_VOID(ARG(index)));
            fail (Error(RE_MISC));
        }
        if (REF(append))
            fail (Error(RE_BAD_REFINES));
        if (REF(allow)) {
            assert(!IS_VOID(ARG(access)));
            fail (Error(RE_BAD_REFINES));
        }
        if (REF(lines))
            fail (Error(RE_BAD_REFINES));

        // Write the entire argument string to the network.
        // The lower level write code continues until done.

        if (
            !GET_FLAG(sock->modes, RST_UDP)
            && !GET_FLAG(sock->state, RSM_CONNECT)
        ){
            fail (Error_On_Port(RE_NOT_CONNECTED, port, -15));
        }

        // Determine length. Clip /PART to size of string if needed.
        REBVAL *data = ARG(data);

        REBCNT len = VAL_LEN_AT(data);
        if (REF(part)) {
            REBCNT n = Int32s(ARG(limit), 0);
            if (n <= len)
                len = n;
        }

        // Setup the write:

        *CTX_VAR(port, STD_PORT_DATA) = *data; // keep it GC safe
        sock->length = len;
        sock->common.data = VAL_BIN_AT(data);
        sock->actual = 0;

        // Note: send can happen immediately
        //
        REBINT result = OS_DO_DEVICE(sock, RDC_WRITE);
        if (result < 0)
            fail (Error_On_Port(RE_WRITE_ERROR, port, sock->error));

        if (result == DR_DONE)
            SET_BLANK(CTX_VAR(port, STD_PORT_DATA));

        *D_OUT = *CTX_VALUE(port);
        return R_OUT; }

    case SYM_PICK: {
        INCLUDE_PARAMS_OF_PICK;
        UNUSED(PAR(aggregate));

        // FIRST server-port returns new port connection.
        //
        REBCNT len = Get_Num_From_Arg(ARG(index));
        if (len == 1 && GET_FLAG(sock->modes, RST_LISTEN) && sock->common.data)
            Accept_New_Port(SINK(D_OUT), port, sock);
        else
            fail (Error_Out_Of_Range(ARG(index)));
        return R_OUT; }

    case SYM_QUERY: {
        //
        // Get specific information - the scheme's info object.
        // Special notation allows just getting part of the info.
        //
        Ret_Query_Net(port, sock, D_OUT);
        return R_OUT; }

    case SYM_OPEN_Q:
        //
        // Connect for clients, bind for servers:
        //
        return R_FROM_BOOL (
            LOGICAL(sock->state & ((1 << RSM_CONNECT) | (1 << RSM_BIND)))
        );

    case SYM_CLOSE: {
        if (IS_OPEN(sock)) {
            OS_DO_DEVICE(sock, RDC_CLOSE);
            SET_CLOSED(sock);
        }
        *D_OUT = *CTX_VALUE(port);
        return R_OUT; }

    case SYM_LENGTH: {
        REBVAL *port_data = CTX_VAR(port, STD_PORT_DATA);
        SET_INTEGER(
            D_OUT,
            ANY_SERIES(port_data) ? VAL_LEN_HEAD(port_data) : 0
        );
        return R_OUT; }

    case SYM_OPEN: {
        REBINT result = OS_DO_DEVICE(sock, RDC_CONNECT);
        if (result < 0)
            fail (Error_On_Port(RE_NO_CONNECT, port, sock->error));
        *D_OUT = *CTX_VALUE(port);
        return R_OUT; }

    case SYM_DELETE: {
        //
        // !!! Comment said "Temporary to TEST error handler!"
        //
        REBVAL *event = Append_Event(); // sets signal
        VAL_RESET_HEADER(event, REB_EVENT); // has more space, if needed
        VAL_EVENT_TYPE(event) = EVT_ERROR;
        VAL_EVENT_DATA(event) = 101;
        VAL_EVENT_REQ(event) = sock;
        *D_OUT = *CTX_VALUE(port);
        return R_OUT; }
    }

    fail (Error_Illegal_Action(REB_PORT, action));
}


//
//  TCP_Actor: C
//
static REB_R TCP_Actor(REBFRM *frame_, REBCTX *port, REBSYM action)
{
    return Transport_Actor(frame_, port, action, TRANSPORT_TCP);
}


//
//  UDP_Actor: C
//
static REB_R UDP_Actor(REBFRM *frame_, REBCTX *port, REBSYM action)
{
    return Transport_Actor(frame_, port, action, TRANSPORT_UDP);
}

//
//  Init_TCP_Scheme: C
//
void Init_TCP_Scheme(void)
{
    Register_Scheme(Canon(SYM_TCP), TCP_Actor);
}

//
//  Init_UDP_Scheme: C
//
void Init_UDP_Scheme(void)
{
    Register_Scheme(Canon(SYM_UDP), UDP_Actor);
}
