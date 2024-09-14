/*
 *
  ***** BEGIN LICENSE BLOCK *****

  Copyright (C) 2023 Olof Hagsand

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, indicate
  your decision by deleting the provisions above and replace them with the
  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****
  *
  * Device handle, hidden C struct and accessor functions
  */
#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <unistd.h>
#include <syslog.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <signal.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include <clixon/clixon.h>

/* Controller includes */
#include "controller.h"
#include "controller_netconf.h"
#include "controller_device_state.h"
#include "controller_device_handle.h"

/*
 * Constants
 */
#define CLIXON_CLIENT_MAGIC 0x54fe649a

#define devhandle(dh) (assert(device_handle_check(dh)==0),(struct controller_device_handle *)(dh))

/*! Internal structure of clixon controller device handle.
 */
struct controller_device_handle{
    qelem_t            cdh_qelem;      /* List header */
    uint32_t           cdh_magic;      /* Magic number */
    char              *cdh_name;       /* Connection name */
    yang_config_t      cdh_yang_config; /* Yang config (shadow of config) */
    conn_state         cdh_conn_state; /* Connection state */
    struct timeval     cdh_conn_time;  /* Time when entering last connection state */
    clixon_handle      cdh_h;          /* Clixon handle */
    clixon_client_type cdh_type;       /* Clixon socket type */
    int                cdh_socket;     /* Input/output socket, -1 is closed */
    int                cdh_sockerr;    /* Stderr socket, -1 is closed */
    uint64_t           cdh_msg_id;     /* Client message-id to device */
    int                cdh_pid;        /* Sub-process-id Only applies for NETCONF/SSH */
    uint64_t           cdh_tid;        /* if >0, dev is part of transaction, 0 means unassigned */
    cbuf              *cdh_frame_buf;  /* Remaining expecting chunk bytes */
    int                cdh_frame_state;/* Framing state for detecting EOM */
    size_t             cdh_frame_size; /* Remaining expecting chunk bytes */
    netconf_framing_type cdh_framing_type; /* Netconf framing type of device */
    cxobj             *cdh_xcaps;      /* Capabilities as XML tree */
    cxobj             *cdh_yang_lib;   /* RFC 8525 yang-library module list */
    struct timeval     cdh_sync_time;  /* Time when last sync (0 if unsynched) */
    int                cdh_nr_schemas; /* How many schemas from this device */
    char              *cdh_schema_name; /* Pending schema name */
    char              *cdh_schema_rev;  /* Pending schema revision */
    char              *cdh_logmsg;      /* Error log message / reason of failed open */
    char              *cdh_domain;      /* YANG domain (for isolation) */
    cbuf              *cdh_outmsg1;     /* Pending outgoing netconf message #1 for delayed output */
    cbuf              *cdh_outmsg2;     /* Pending outgoing netconf message #2 for delayed output */
};

/*! Check struct magic number for sanity checks
 *
 * @param[in]  dh  Device handle
 * @retval     0   Sanity check OK
 * @retval    -1   Sanity check failed
 */
static int
device_handle_check(device_handle dh)
{
    /* Dont use handle macro to avoid recursion */
    struct controller_device_handle *cdh = (struct controller_device_handle *)(dh);

    return cdh->cdh_magic == CLIXON_CLIENT_MAGIC ? 0 : -1;
}

/*! Free handle itself
 *
 * @param[in]  dh  Controller device handle
 * @retval     0   OK
 */
static int
device_handle_free1(struct controller_device_handle *cdh)
{
    if (cdh->cdh_name)
        free(cdh->cdh_name);
    if (cdh->cdh_frame_buf)
        cbuf_free(cdh->cdh_frame_buf);
    if (cdh->cdh_xcaps)
        xml_free(cdh->cdh_xcaps);
    if (cdh->cdh_yang_lib)
        xml_free(cdh->cdh_yang_lib);
    if (cdh->cdh_logmsg)
        free(cdh->cdh_logmsg);
    if (cdh->cdh_schema_name)
        free(cdh->cdh_schema_name);
    if (cdh->cdh_schema_rev)
        free(cdh->cdh_schema_rev);
    if (cdh->cdh_domain)
        free(cdh->cdh_domain);
    if (cdh->cdh_outmsg1)
        cbuf_free(cdh->cdh_outmsg1);
    if (cdh->cdh_outmsg2)
        cbuf_free(cdh->cdh_outmsg2);
    free(cdh);
    return 0;
}

/*! Create new controller device handle given clixon handle and add it to global list
 *
 * A new device handle is created when a connection is made, also passively in
 * controller_yang_mount
 * @param[in]  h    Clixon  handle
 * @retval     dh   Controller device handle
 */
device_handle
device_handle_new(clixon_handle h,
                  const char   *name)
{
    struct controller_device_handle *cdh = NULL;
    struct controller_device_handle *cdh_list = NULL;
    size_t                           sz;

    clixon_debug(CLIXON_DBG_CTRL, "");
    sz = sizeof(struct controller_device_handle);
    if ((cdh = malloc(sz)) == NULL){
        clixon_err(OE_NETCONF, errno, "malloc");
        return NULL;
    }
    memset(cdh, 0, sz);
    cdh->cdh_magic = CLIXON_CLIENT_MAGIC;
    cdh->cdh_h = h;
    cdh->cdh_socket = -1;
    cdh->cdh_sockerr = -1;
    cdh->cdh_conn_state = CS_CLOSED;
    if ((cdh->cdh_name = strdup(name)) == NULL){
        clixon_err(OE_UNIX, errno, "strdup");
        device_handle_free1(cdh);
        return NULL;
    }
    if ((cdh->cdh_frame_buf = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        device_handle_free1(cdh);
        return NULL;
    }
    (void)clicon_ptr_get(h, "client-list", (void**)&cdh_list);
    ADDQ(cdh, cdh_list);
    clicon_ptr_set(h, "client-list", (void*)cdh_list);
    return cdh;
}


/*! Free controller device handle
 *
 * @param[in]  dh   Device handle
 * @retval     0    OK
 */
int
device_handle_free(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);
    struct controller_device_handle *cdh_list = NULL;
    struct controller_device_handle *c;
    clixon_handle                    h;

    h = (clixon_handle)cdh->cdh_h;
    clicon_ptr_get(h, "client-list", (void**)&cdh_list);
    if ((c = cdh_list) != NULL) {
        do {
            if (cdh == c) {
                DELQ(c, cdh_list, struct controller_device_handle *);
                device_handle_free1(c);
                break;
            }
            c = NEXTQ(struct controller_device_handle *, c);
        } while (c && c != cdh_list);
    }
    clicon_ptr_set(h, "client-list", (void*)cdh_list);
    return 0;
}

/*! Free all controller's device handles
 *
 * @param[in]  h   Clixon handle
 */
int
device_handle_free_all(clixon_handle h)
{
    struct controller_device_handle *cdh_list = NULL;
    struct controller_device_handle *c;

    clicon_ptr_get(h, "client-list", (void**)&cdh_list);
    while ((c = cdh_list) != NULL) {
        DELQ(c, cdh_list, struct controller_device_handle *);
        device_handle_free1(c);
    }
    clicon_ptr_set(h, "client-list", (void*)cdh_list);
    return 0;
}

/*! Find clixon-client given name
 *
 * @param[in]  h     Clixon  handle
 * @param[in]  name  Client name
 * @retval     dh    Device handle
 */
device_handle
device_handle_find(clixon_handle h,
                   const char   *name)
{
    struct controller_device_handle *cdh_list = NULL;
    struct controller_device_handle *c = NULL;

    if (clicon_ptr_get(h, "client-list", (void**)&cdh_list) == 0 &&
        (c = cdh_list) != NULL) {
        do {
            if (strcmp(c->cdh_name, name) == 0)
                return c;
            c = NEXTQ(struct controller_device_handle *, c);
        } while (c && c != cdh_list);
    }
    return NULL;
}

/*! Iterator over device-handles
 *
 * @param[in]  h      Clixon handle
 * @param[in]  dhprev iteration handle, init with NULL
 * @code
 *    device_handle dh = NULL;
 *    while ((dh = device_handle_each(h, dh)) != NULL){
 *       dh...
 * @endcode
 */
device_handle
device_handle_each(clixon_handle h,
                   device_handle dhprev)
{
    struct controller_device_handle *cdh = (struct controller_device_handle *)dhprev;
    struct controller_device_handle *cdh0 = NULL;

    clicon_ptr_get(h, "client-list", (void**)&cdh0);
    if (cdh == NULL)
        return cdh0;
    cdh = NEXTQ(struct controller_device_handle *, cdh);
    if (cdh == cdh0)
        return NULL;
    else
        return cdh;
}

/*! Connect client to clixon backend according to config and return a socket
 *
 * @param[in]  h        Clixon handle
 * @param[in]  socktype Type of socket, internal/external/netconf/ssh
 * @param[in]  dest     Destination for some types
 * @param[in]  stricthostkey If set ensure strict hostkey checking. Only for ssh connections
 * @retval     dh       Clixon session handler
 * @retval     NULL     Error
 * @see clixon_client_disconnect  Close the socket returned here
 */
int
device_handle_connect(device_handle      dh,
                      clixon_client_type socktype,
                      const char        *dest,
                      int                stricthostkey)
{
    int                              retval = -1;
    struct controller_device_handle *cdh = (struct controller_device_handle *)dh;
    clixon_handle                    h;

    clixon_debug(CLIXON_DBG_CTRL, "");
    if (cdh == NULL){
        clixon_err(OE_XML, EINVAL, "dh is NULL");
        goto done;
    }
    h = cdh->cdh_h;
    cdh->cdh_type = socktype;
    switch (socktype){
    case CLIXON_CLIENT_IPC:
        if (clicon_rpc_connect(h, &cdh->cdh_socket) < 0)
            goto err;
        break;
    case CLIXON_CLIENT_NETCONF:
        if (clixon_client_connect_netconf(h, &cdh->cdh_pid, &cdh->cdh_socket) < 0)
            goto err;
        break;
#ifdef SSH_BIN
    case CLIXON_CLIENT_SSH:
        if (clixon_client_connect_ssh(h, dest, stricthostkey, &cdh->cdh_pid, &cdh->cdh_socket, &cdh->cdh_sockerr) < 0)
            goto err;
#else
        clixon_err(OE_UNIX, 0, "No ssh bin");
        goto done;
#endif
        break;
    } /* switch */
    retval = 0;
 done:
    clixon_debug(CLIXON_DBG_CTRL, "retval:%d", retval);
    return retval;
 err:
    if (cdh)
        clixon_client_disconnect(cdh);
    goto done;
}

/*! Connect client to clixon backend according to config and return a socket
 *
 * @param[in]  dh   Clixon client session handle
 * @retval     0    OK
 * @retval    -1    Error
 * @see clixon_client_connect where the handle is created
 * The handle is deallocated
 */
int
device_handle_disconnect(device_handle dh)
{
    int                              retval = -1;
    struct controller_device_handle *cdh = devhandle(dh);

    if (cdh == NULL){
        clixon_err(OE_XML, EINVAL, "Expected cdh handle");
        goto done;
    }
    clixon_debug(CLIXON_DBG_CTRL, "%s", cdh->cdh_name);
    switch(cdh->cdh_type){
    case CLIXON_CLIENT_IPC:
        close(cdh->cdh_socket);
        cdh->cdh_socket = -1;
        break;
    case CLIXON_CLIENT_SSH:
    case CLIXON_CLIENT_NETCONF:
        assert(cdh->cdh_pid && cdh->cdh_socket != -1);
        if (cdh->cdh_sockerr != -1){
            close(cdh->cdh_sockerr);
            cdh->cdh_sockerr = -1;
        }
        if (clixon_proc_socket_close(cdh->cdh_pid, cdh->cdh_socket) < 0)
            goto done;
        cdh->cdh_pid = 0;
        cdh->cdh_socket = -1;
        break;
    }
    retval = 0;
 done:
    clixon_debug(CLIXON_DBG_CTRL, "retval:%d", retval);
    return retval;
}


/* Accessor functions ------------------------------
 */
/*! Get name of connection, allocated at creation time
 */
char*
device_handle_name_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_name;
}

/*! Get socket
 *
 * @param[in]  dh     Device handle
 * @retval     s      Open socket
 * @retval    -1      No/closed socket
 */
int
device_handle_socket_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_socket;
}

/*! Get err socket
 *
 * @param[in]  dh     Device handle
 * @retval     s      Open error socket
 * @retval    -1      No/closed socket
 */
int
device_handle_sockerr_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_sockerr;
}

/*! Get msg-id and increment
 *
 * @param[in]  dh     Device handle
 * @retval     msgid
 */
uint64_t
device_handle_msg_id_getinc(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_msg_id++;
}

/*! Get transaction id
 *
 * @param[in]  dh     Device handle
 * @retval     tid    Transaction-id (0 means unassigned)
 */
uint64_t
device_handle_tid_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_tid;
}

/*! Get transaction id
 *
 * @param[in]  dh     Device handle
 * @param[in]  tid    Transaction-id (0 means unassigned)
 */
int
device_handle_tid_set(device_handle dh,
                      uint64_t      tid)
{
    struct controller_device_handle *cdh = devhandle(dh);

    cdh->cdh_tid = tid;
    return 0;
}

clixon_handle
device_handle_handle_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_h;
}

/*! Get yang config
 *
 * @param[in]  dh          Device handle
 * @retval     yang-config How to bind device configuration to YANG
 * @note mirror of config
 */
yang_config_t
device_handle_yang_config_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_yang_config;
}

/*! Set yang config
 *
 * @param[in]  dh     Device handle
 * @param[in]  yfstr  Yang config setting as string
 * @retval     0      OK
 * @note mirror of config, only commit callback code should set this value
 */
int
device_handle_yang_config_set(device_handle dh,
                              char         *yfstr)
{
    struct controller_device_handle *cdh = devhandle(dh);
    yang_config_t                    yf;

    yf = yang_config_str2int(yfstr);
    cdh->cdh_yang_config = yf;
    return 0;
}

/*! Get connection state
 *
 * @param[in]  dh     Device handle
 * @retval     state
 */
conn_state
device_handle_conn_state_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_conn_state;
}

/*! Set connection state also timestamp
 *
 * @param[in]  dh     Device handle
 * @param[in]  state  State
 * @retval     0      OK
 */
int
device_handle_conn_state_set(device_handle dh,
                             conn_state    state)
{
    struct controller_device_handle *cdh = devhandle(dh);

    assert(device_state_int2str(state)!=NULL);
    clixon_debug(CLIXON_DBG_CTRL, "%s: %s -> %s",
                 device_handle_name_get(dh),
                 device_state_int2str(cdh->cdh_conn_state),
                 device_state_int2str(state));
    /* Free logmsg if leaving closed */
    if (cdh->cdh_conn_state == CS_CLOSED &&
        cdh->cdh_logmsg){
        free(cdh->cdh_logmsg);
        cdh->cdh_logmsg = NULL;
    }
    cdh->cdh_conn_state = state;
    device_handle_conn_time_set(dh, NULL);
    return 0;
}

/*! Get connection timestamp
 *
 * @param[in]  dh     Device handle
 * @param[out] t      Connection timestamp
 */
int
device_handle_conn_time_get(device_handle   dh,
                            struct timeval *t)
{
    struct controller_device_handle *cdh = devhandle(dh);

    *t = cdh->cdh_conn_time;
    return 0;
}

/*! Set connection timestamp
 *
 * @param[in]  dh     Device handle
 * @param[in]  t      Timestamp, if NULL set w gettimeofday
 */
int
device_handle_conn_time_set(device_handle   dh,
                            struct timeval *t)
{
    struct controller_device_handle *cdh = devhandle(dh);

    if (t == NULL)
        gettimeofday(&cdh->cdh_conn_time, NULL);
    else
        cdh->cdh_conn_time = *t;
    return 0;
}

/*! Access frame state get
 *
 * @param[in]  dh     Device handle
 * @retval     state
 */
int
device_handle_frame_state_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_frame_state;
}

/*! Access state get
 *
 * @param[in]  dh     Device handle
 * @retval     state  State
 * @retval     0      OK
 */
int
device_handle_frame_state_set(device_handle dh,
                              int           state)
{
    struct controller_device_handle *cdh = devhandle(dh);

    cdh->cdh_frame_state = state;
    return 0;
}

/*! Get Netconf frame size, part of dynamic framing detection
 *
 * @param[in]  dh    Device handle
 * @retval     fs    Frame size
 */
size_t
device_handle_frame_size_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_frame_size;
}

/*! Set Netconf frame size, part of dynamic framing detection
 *
 * @param[in]  dh     Device handle
 * @param[in]  size   Frame size
 */
int
device_handle_frame_size_set(device_handle dh,
                             size_t        size)
{
    struct controller_device_handle *cdh = devhandle(dh);

    cdh->cdh_frame_size = size;
    return 0;
}

/*! Get Netconf framing type of device
 *
 * @param[in]  dh     Device handle
 */
cbuf *
device_handle_frame_buf_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_frame_buf;
}

/*! Set Netconf framing type of device
 *
 * @param[in]  dh   Device handle
 * @retval     ft   Framing type
 */
netconf_framing_type
device_handle_framing_type_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_framing_type;
}

/*! Get Netconf framing type of device
 *
 * @param[in]  dh  Device handle
 * @param[in]  ft  Framing type
 */
int
device_handle_framing_type_set(device_handle        dh,
                               netconf_framing_type ft)
{
    struct controller_device_handle *cdh = devhandle(dh);

    cdh->cdh_framing_type = ft;
    return 0;
}

/*! Get capabilities as xml tree
 *
 * @param[in]  dh     Device handle
 * @retval     xcaps  XML tree
 */
cxobj *
device_handle_capabilities_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_xcaps;
}

/*! Set capabilities as xml tree
 *
 * @param[in]  dh     Device handle
 * @param[in]  xcaps  XML tree, is consumed
 * @retval     0      OK
 */
int
device_handle_capabilities_set(device_handle dh,
                               cxobj        *xcaps)
{
    struct controller_device_handle *cdh = devhandle(dh);

    if (cdh->cdh_xcaps != NULL)
        xml_free(cdh->cdh_xcaps);
    cdh->cdh_xcaps = xcaps;
    return 0;
}

/*! Query if capabaility exists on device, match uri
 *
 * @param[in]  dh    Device handle
 * @param[in]  name  Capability name
 * @retval     1     Yes, capability exists
 * @retval     0     No, capabilty does not exist
 */
int
device_handle_capabilities_find(clixon_handle dh,
                                const char   *name)
{
    struct controller_device_handle *cdh = devhandle(dh);
    cxobj                           *xcaps = NULL;
    cxobj                           *x = NULL;
    char                            *b;
    char                            *bi;

    xcaps = cdh->cdh_xcaps;
    while ((x = xml_child_each(xcaps, x, -1)) != NULL) {
        b = xml_body(x);
        if ((bi = index(b, '?')) != NULL){
            if (strncmp(name, b, bi-b) == 0)
                break;
        }
        else
            if (strcmp(name, b) == 0)
                break;
    }
    return x?1:0;
}

/*! Get RFC 8525 yang-lib as xml tree
 *
 * @param[in]  dh     Device handle
 * @retval     yang_lib  XML tree
 * On the form: yang-library/module-set/name=<name>/module/name,revision,namespace  RFC 8525
 */
cxobj *
device_handle_yang_lib_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_yang_lib;
}

/*! Set RFC 8525 yang library as xml tree
 *
 * @param[in]  dh     Device handle
 * @param[in]  xylib  XML tree, is consumed
 * @retval     0      OK
 * On the form: yang-library/module-set/name=<name>/module/name,revision,namespace  RFC 8525
 * @see  device_handle_yang_lib_append adds yangs
 */
int
device_handle_yang_lib_set(device_handle dh,
                           cxobj        *xylib)
{
    struct controller_device_handle *cdh = devhandle(dh);

    /* Sanity check */
    if (xylib)
        assert(xml_find_type(xylib, NULL, "module-set", CX_ELMNT));
    if (cdh->cdh_yang_lib != NULL)
        xml_free(cdh->cdh_yang_lib);
    cdh->cdh_yang_lib = xylib;
    return 0;
}

/*! Set RFC 8525 yang library as xml tree
 *
 * @param[in]  dh     Device handle
 * @param[in]  xylib  XML tree to append merge with existing if any (is consumed)
 * @retval     0      OK
 * @retval    -1      Error
 * On the form: yang-library/module-set/name=<name>/module/name,revision,namespace  RFC 8525
 */
int
device_handle_yang_lib_append(device_handle dh,
                              cxobj        *xylib)
{
    int                              retval = -1;
    struct controller_device_handle *cdh = devhandle(dh);
    cxobj                           *xms0;
    cxobj                           *xm0;
    cxobj                           *xms1 = NULL;
    cxobj                           *xm1;
    char                            *name;

    /* Sanity check */
    if (xylib){
        if ((xms1 = xml_find_type(xylib, NULL, "module-set", CX_ELMNT)) == NULL){
            clixon_err(OE_XML, EINVAL, "yang-lib top-level malformed: not module-set");
            goto done;
        }
    }
    if (cdh->cdh_yang_lib) {
        if (xylib){
            if ((xms0 = xml_find_type(cdh->cdh_yang_lib, NULL, "module-set", CX_ELMNT)) == NULL){
                clixon_err(OE_XML, EINVAL, "yang-lib top-level malformed: not module-set");
                goto done;
            }
            if (xml_tree_equal(xms0, xms1) != 0) {
                xm1 = NULL;
                while ((xm1 = xml_child_each(xms1, xm1, CX_ELMNT)) != NULL) {
                    if (strcmp(xml_name(xm1), "module") != 0)
                        continue;
                    if ((name = xml_find_body(xm1, "name")) == NULL)
                        continue;
                    if ((xm0 = xpath_first(xms0, NULL, "module[name='%s']", name)) != NULL){
                        if (xml_tree_equal(xm0, xm1) != 0) {
                            if (xml_rm_children(xm0, -1) < 0)
                                goto done;
                            if (xml_copy(xm1, xm0) < 0)
                                goto done;
                        }
                    }
                    else {
                        if ((xm0 = xml_dup(xm1)) == NULL)
                            goto done;
                        if (xml_addsub(xms0, xm0) < 0)
                            goto done;
                    }
                }
            }
        }
    }
    else{
        cdh->cdh_yang_lib = xylib;
        xylib = NULL;
    }
    retval = 0;
 done:
    if (xylib)
        xml_free(xylib);
    return retval;
}

/*! Get sync timestamp
 *
 * @param[in]  dh     Device handle
 * @param[out] t      Sync timestamp (=0 if uninitialized)
 */
int
device_handle_sync_time_get(device_handle    dh,
                             struct timeval *t)
{
    struct controller_device_handle *cdh = devhandle(dh);

    *t = cdh->cdh_sync_time;
    return 0;
}

/*! Set sync timestamp
 *
 * @param[in]  dh     Device handle
 * @param[in]  t      Timestamp, if NULL set w gettimeofday
 */
int
device_handle_sync_time_set(device_handle   dh,
                            struct timeval *t)
{
    struct controller_device_handle *cdh = devhandle(dh);

    if (t == NULL)
        gettimeofday(&cdh->cdh_sync_time, NULL);
    else
        cdh->cdh_sync_time = *t;
    return 0;
}

/*! Get nr of schemas
 *
 * @param[in]  dh     Device handle
 * @retval     nr
 */
int
device_handle_nr_schemas_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_nr_schemas;
}

/*! Set nr of schemas
 *
 * @param[in]  dh   Device handle
 * @param[in]  nr   Number of schemas
 */
int
device_handle_nr_schemas_set(device_handle dh,
                             int           nr)
{
    struct controller_device_handle *cdh = devhandle(dh);

    cdh->cdh_nr_schemas = nr;
    return 0;
}

/*! Get pending schema name, strdup
 *
 * @param[in]  dh     Device handle
 * @retval     schema-name
 * @retval     NULL
 */
char*
device_handle_schema_name_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_schema_name;
}

/*! Set pending schema name, strdup
 *
 * @param[in]  dh     Device handle
 * @param[in]  schema-name Is copied
 */
int
device_handle_schema_name_set(device_handle dh,
                              char        *schema_name)
{
    struct controller_device_handle *cdh = devhandle(dh);

    if (cdh->cdh_schema_name)
        free(cdh->cdh_schema_name);
    cdh->cdh_schema_name = strdup(schema_name);
    return 0;
}

/*! Get pending schema rev, strdup
 *
 * @param[in]  dh     Device handle
 * @retval     schema-rev
 * @retval     NULL
 */
char*
device_handle_schema_rev_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_schema_rev;
}

/*! Set pending schema rev, strdup
 *
 * @param[in]  dh     Device handle
 * @param[in]  schema-rev Is copied
 */
int
device_handle_schema_rev_set(device_handle dh,
                              char        *schema_rev)
{
    struct controller_device_handle *cdh = devhandle(dh);

    if (cdh->cdh_schema_rev)
        free(cdh->cdh_schema_rev);
    cdh->cdh_schema_rev = strdup(schema_rev);
    return 0;
}

/*! Get logmsg, direct pointer into struct
 *
 * @param[in]  dh     Device handle
 * @retval     logmsg
 * @retval     NULL
 */
char*
device_handle_logmsg_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_logmsg;
}

/*! Set logmsg, consume string
 *
 * @param[in]  dh     Device handle
 * @param[in]  logmsg Logmsg (is consumed)
 */
int
device_handle_logmsg_set(device_handle dh,
                         char        *logmsg)
{
    struct controller_device_handle *cdh = devhandle(dh);

    if (cdh->cdh_logmsg)
        free(cdh->cdh_logmsg);
    cdh->cdh_logmsg = logmsg;
    return 0;
}

/*! Get YANG domain name
 *
 * @param[in]  dh     Device handle
 * @retval     domain
 * @retval     NULL
 */
char*
device_handle_domain_get(device_handle dh)
{
    struct controller_device_handle *cdh = devhandle(dh);

    return cdh->cdh_domain;
}

/*! Set YANG domain name
 *
 * @param[in]  dh     Device handle
 * @param[in]  domain YANG domain name
 */
int
device_handle_domain_set(device_handle dh,
                         char         *domain)
{
    struct controller_device_handle *cdh = devhandle(dh);

    if (cdh->cdh_domain)
        free(cdh->cdh_domain);
    cdh->cdh_domain = strdup(domain);
    return 0;
}

/*! Get pending netconf outmsg
 *
 * @param[in]  dh     Device handle
 * @param[in]  nr     Message nr, 1 or 2
 * @retval     msg
 * @retval     NULL
 */
cbuf*
device_handle_outmsg_get(device_handle dh,
                         int           nr)
{
    struct controller_device_handle *cdh = devhandle(dh);

    if (nr == 1)
        return cdh->cdh_outmsg1;
    else if (nr == 2)
        return cdh->cdh_outmsg2;
    else
        return NULL;
}

/*! Set pending netconf outmsg #1
 *
 * @param[in]  dh   Device handle
 * @param[in]  nr   Message nr, 1 or 2
 * @param[in]  cb   Netconf msg (is consumed)
 * @retval     0    OK
 * @retval    -1    Error
 */
int
device_handle_outmsg_set(device_handle dh,
                         int           nr,
                         cbuf         *cb)
{
    struct controller_device_handle *cdh = devhandle(dh);

    if (nr != 1 && nr != 2){
        clixon_err(OE_XML, EINVAL, "nr must be 1 or 2");
        return -1;
    }
    if (nr == 1){
        if (cdh->cdh_outmsg1){
            cbuf_free(cdh->cdh_outmsg1);
            cdh->cdh_outmsg1 = NULL;
        }
        cdh->cdh_outmsg1 = cb;
    }
    else if (nr == 2) {
        if (cdh->cdh_outmsg2){
            cbuf_free(cdh->cdh_outmsg2);
            cdh->cdh_outmsg2 = NULL;
        }
        cdh->cdh_outmsg2 = cb;
    }
    return 0;
}
