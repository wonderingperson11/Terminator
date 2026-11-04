/*
 * proto.h - the message protocol between the GUI and a tab's shell.
 *
 * Every tab, in thread mode and in process mode alike, is connected to the
 * GUI by two pipes.  Messages are framed as a fixed header followed by a
 * payload, so the same code runs whether the other end is a thread in this
 * process or a separate forked process.
 */
#ifndef TERMINATOR_PROTO_H
#define TERMINATOR_PROTO_H

#include <stddef.h>
#include <stdint.h>

/* GUI -> shell */
enum {
    M_KEY = 1,    /* int32 action, then optional UTF-8 text (ACT_INSERT)  */
    M_RESIZE = 2, /* int32 cols, int32 rows                               */
};

/* shell -> GUI */
enum {
    M_OUT = 10,       /* raw output bytes to append to the scrollback      */
    M_COMMIT = 11,    /* int32 prompt_len, then prompt+line: a finished
                         input line, appended with a newline               */
    M_EDIT = 12,      /* int32 prompt_len (-1 = hide), int32 cursor (byte
                         offset into the input part), then prompt+input    */
    M_CLEAR = 13,     /* clear the scrollback (Ctrl+L / clear)             */
    M_TITLE = 14,     /* tab title (the shell's working directory)         */
    M_BINDRELOAD = 15,/* ~/.terminatorrc changed: reload the key bindings  */
    M_EXIT = 16,      /* int32 status: the shell is exiting normally       */
};

/* Editing / control actions carried by M_KEY. */
typedef enum {
    ACT_NONE = 0,
    ACT_INSERT,       /* text payload                                      */
    ACT_ENTER,
    ACT_BACKSPACE,
    ACT_DELETE,
    ACT_LEFT,
    ACT_RIGHT,
    ACT_HOME,
    ACT_END,
    ACT_HIST_PREV,
    ACT_HIST_NEXT,
    ACT_KILL_START,
    ACT_KILL_END,
    ACT_DEL_WORD,
    ACT_CLEAR,
    ACT_EOF,
    ACT_INTR,
    ACT_SUSP,
    ACT_SEARCH,
    ACT_COMPLETE,
    ACT_ESCAPE,
    ACT__COUNT
} Action;

typedef struct {
    uint32_t type;
    uint32_t len;
} MsgHdr;

/* Blocking send of one framed message (header + up to two payload parts). */
int msg_send(int fd, int type, const void *a, size_t alen, const void *b, size_t blen);
int msg_send_int(int fd, int type, int32_t v, const void *text, size_t tlen);
int msg_send_int2(int fd, int type, int32_t v1, int32_t v2, const void *text, size_t tlen);

/* Incremental reader for a non-blocking pipe. */
typedef struct {
    char *buf;
    size_t len, cap, off;
} MsgReader;

void mr_init(MsgReader *mr);
void mr_free(MsgReader *mr);
/* Read what is available: returns >0 bytes read, 0 on EOF, -1 if nothing
 * available (EAGAIN) or on error. */
long mr_fill(MsgReader *mr, int fd);
/* Extract the next complete message.  Returns 1 and fills the outputs
 * (data valid until the next mr_* call), or 0 if none is complete. */
int mr_next(MsgReader *mr, uint32_t *type, char **data, uint32_t *len);
int mr_pending(const MsgReader *mr);

int32_t msg_int(const char *data, int idx);

#endif
