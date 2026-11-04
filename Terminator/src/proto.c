/*
 * proto.c - framing for the GUI <-> shell pipes (see proto.h).
 */
#include "proto.h"
#include "util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

int msg_send(int fd, int type, const void *a, size_t alen, const void *b, size_t blen)
{
    MsgHdr h;
    h.type = (uint32_t)type;
    h.len = (uint32_t)(alen + blen);
    /* One buffer => one write() for small messages, which keeps a frame
     * contiguous in the pipe. */
    size_t total = sizeof h + alen + blen;
    char stackbuf[4096];
    char *p = total <= sizeof stackbuf ? stackbuf : xmalloc(total);
    memcpy(p, &h, sizeof h);
    if (alen)
        memcpy(p + sizeof h, a, alen);
    if (blen)
        memcpy(p + sizeof h + alen, b, blen);
    int r = write_all(fd, p, total);
    if (p != stackbuf)
        free(p);
    return r;
}

int msg_send_int(int fd, int type, int32_t v, const void *text, size_t tlen)
{
    return msg_send(fd, type, &v, sizeof v, text, tlen);
}

int msg_send_int2(int fd, int type, int32_t v1, int32_t v2, const void *text, size_t tlen)
{
    int32_t v[2] = { v1, v2 };
    return msg_send(fd, type, v, sizeof v, text, tlen);
}

void mr_init(MsgReader *mr)
{
    memset(mr, 0, sizeof *mr);
}

void mr_free(MsgReader *mr)
{
    free(mr->buf);
    mr_init(mr);
}

long mr_fill(MsgReader *mr, int fd)
{
    if (mr->off > 0) {
        memmove(mr->buf, mr->buf + mr->off, mr->len - mr->off);
        mr->len -= mr->off;
        mr->off = 0;
    }
    if (mr->cap - mr->len < 65536) {
        mr->cap = mr->cap ? mr->cap * 2 : 131072;
        mr->buf = xrealloc(mr->buf, mr->cap);
    }
    ssize_t r = read_retry(fd, mr->buf + mr->len, mr->cap - mr->len);
    if (r > 0)
        mr->len += (size_t)r;
    if (r < 0)
        return -1;
    return (long)r;
}

int mr_next(MsgReader *mr, uint32_t *type, char **data, uint32_t *len)
{
    size_t avail = mr->len - mr->off;
    if (avail < sizeof(MsgHdr))
        return 0;
    MsgHdr h;
    memcpy(&h, mr->buf + mr->off, sizeof h);
    if (avail < sizeof h + h.len)
        return 0;
    *type = h.type;
    *len = h.len;
    *data = mr->buf + mr->off + sizeof h;
    mr->off += sizeof h + h.len;
    return 1;
}

int mr_pending(const MsgReader *mr)
{
    size_t avail = mr->len - mr->off;
    if (avail < sizeof(MsgHdr))
        return 0;
    MsgHdr h;
    memcpy(&h, mr->buf + mr->off, sizeof h);
    return avail >= sizeof h + h.len;
}

int32_t msg_int(const char *data, int idx)
{
    int32_t v;
    memcpy(&v, data + idx * 4, sizeof v);
    return v;
}
