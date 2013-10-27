
/**
 * Copyright (c) 2011, Willem-Hendrik Thiart
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file. 
 *
 * @file
 * @brief Manage a connection with a peer
 * @author  Willem Thiart himself@willemthiart.com
 * @version 0.1
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* for uint32_t */
#include <stdint.h>

/* for varags */
#include <stdarg.h>

#include "bitfield.h"
#include "pwp_connection.h"
#include "pwp_local.h"
#include "linked_list_hashmap.h"
#include "linked_list_queue.h"
#include "bitstream.h"
#include "meanqueue.h"

#define TRUE 1
#define FALSE 0

#define pwp_msgtype_to_string(m)\
    PWP_MSGTYPE_CHOKE == (m) ? "CHOKE" :\
    PWP_MSGTYPE_UNCHOKE == (m) ? "UNCHOKE" :\
    PWP_MSGTYPE_INTERESTED == (m) ? "INTERESTED" :\
    PWP_MSGTYPE_UNINTERESTED == (m) ? "UNINTERESTED" :\
    PWP_MSGTYPE_HAVE == (m) ? "HAVE" :\
    PWP_MSGTYPE_BITFIELD == (m) ? "BITFIELD" :\
    PWP_MSGTYPE_REQUEST == (m) ? "REQUEST" :\
    PWP_MSGTYPE_PIECE == (m) ? "PIECE" :\
    PWP_MSGTYPE_CANCEL == (m) ? "CANCEL" : "none"\

/*  state */
typedef struct
{
    /* this bitfield indicates which pieces the peer has */
    bitfield_t have_bitfield;

    /* for recording state machine's state */
    unsigned int flags;

    /* count number of failed connections */
    int failed_connections;

    /* current tick */
    int tick;

} peer_connection_state_t;

typedef struct
{
    /* the tick which this request was made */
    int tick;
    bt_block_t blk;
} request_t;

/*  peer connection */
typedef struct
{
    peer_connection_state_t state;

    unsigned int bytes_downloaded_this_period;
    unsigned int bytes_uploaded_this_period;
    void* bytes_downloaded_rate;
    void* bytes_uploaded_rate;

    /*  requests that we are waiting to get */
    hashmap_t *pendreqs;

    /* requests we are fufilling for the peer */
    linked_list_queue_t *pendpeerreqs;

    int piece_len;
    int num_pieces;

    /* info of who we are connected to */
    void *peer_udata;

    /* callbacks */
    pwp_conn_functions_t *func;
    void *caller;

} pwp_conn_private_t;

/**
 * Flip endianess
 **/
static uint32_t fe(uint32_t i)
{
    uint32_t o;
    unsigned char *c = (unsigned char *)&i;
    unsigned char *p = (unsigned char *)&o;

    p[0] = c[3];
    p[1] = c[2];
    p[2] = c[1];
    p[3] = c[0];

    return o;
}

static unsigned long __request_hash(const void *obj)
{
    const bt_block_t *req = obj;

    return req->piece_idx + req->len + req->offset;
}

static long __request_compare(const void *obj, const void *other)
{
    const bt_block_t *req1 = obj, *req2 = other;

    if (req1->piece_idx == req2->piece_idx &&
        req1->len == req2->len &&
        req1->offset == req2->offset)
        return 0;

    return 1;
}

static void __log(pwp_conn_private_t * me, const char *format, ...)
{
    char buffer[1000];

    va_list args;

    if (NULL == me->func || NULL == me->func->log)
        return;

    va_start(args, format);
    (void)vsnprintf(buffer, 1000, format, args);

//    printf("%s\n", buffer);

    me->func->log(me->caller, me->peer_udata, buffer);
}

static void __disconnect(pwp_conn_private_t * me, const char *reason, ...)
{
    char buffer[128];

    va_list args;

    va_start(args, reason);
    (void)vsnprintf(buffer, 128, reason, args);
    if (me->func->disconnect)
    {
       (void)me->func->disconnect(me->caller, me->peer_udata, buffer);
    }
}

static int __send_to_peer(pwp_conn_private_t * me, void *data, const int len)
{
    int ret;

    if (NULL != me->func && NULL != me->func->send)
    {
        ret = me->func->send(me->caller, me->peer_udata, data, len);

        if (0 == ret)
        {
            __disconnect(me, "peer dropped connection");
            return 0;
        }
    }

    return 1;
}

void *pwp_conn_get_peer(pwp_conn_t* me_)
{
    pwp_conn_private_t *me = (void*)me_;

    return me->peer_udata;
}

void pwp_conn_set_peer(pwp_conn_t* me_, void * peer)
{
    pwp_conn_private_t *me = (void*)me_;

    me->peer_udata = peer;
}

void *pwp_conn_new()
{
    pwp_conn_private_t *me;

    if(!(me = calloc(1, sizeof(pwp_conn_private_t))))
    {
        perror("out of memory");
        exit(0);
    }

    me->state.flags = PC_IM_CHOKING | PC_PEER_CHOKING;
    me->pendreqs = hashmap_new(__request_hash, __request_compare, 100);
    me->pendpeerreqs = llqueue_new();
    me->bytes_downloaded_rate = meanqueue_new(10);
    me->bytes_uploaded_rate = meanqueue_new(10);
    return me;
}

static void __expunge_their_pending_reqs(pwp_conn_private_t* me)
{
    while (0 < llqueue_count(me->pendpeerreqs))
    {
        free(llqueue_poll(me->pendpeerreqs));
    }
}

static void __expunge_my_pending_reqs(pwp_conn_private_t* me)
{
    request_t *r;
    hashmap_iterator_t iter;

    void* rem;
    rem = llqueue_new();

    for (hashmap_iterator(me->pendreqs, &iter);
         (r = hashmap_iterator_next_value(me->pendreqs, &iter));)
    {
        llqueue_offer(rem, r);
#if 0
        r = hashmap_remove(me->pendreqs, &r->blk);
        if (me->func->peer_giveback_block)
            me->func->peer_giveback_block(me->caller, me->peer_udata, &r->blk);
        free(r);
#endif
    }

#if 1
   while (llqueue_count(rem) > 0)
    {
        r = llqueue_poll(rem);
        r = hashmap_remove(me->pendreqs, &r->blk);

        assert(r);

        if (me->func->peer_giveback_block)
            me->func->peer_giveback_block(me->caller, me->peer_udata, &r->blk);
        free(r);
    }

    llqueue_free(rem);
#endif
}

static void __expunge_my_old_pending_reqs(pwp_conn_private_t* me)
{
    request_t *r;
    hashmap_iterator_t iter;
    void* rem;

    rem = llqueue_new();

    for (hashmap_iterator(me->pendreqs, &iter);
         (r = hashmap_iterator_next_value(me->pendreqs, &iter));)
    {
        if (r && 10 < me->state.tick - r->tick)
        {
#if 0
            hashmap_remove(me->pendreqs, &r->blk);
            me->func->peer_giveback_block(me->caller, me->peer_udata, &r->blk);
            free(r);
#endif
            llqueue_offer(rem, r);
        }
    }

#if 1
    while (llqueue_count(rem) > 0)
    {
        r = llqueue_poll(rem);
        r = hashmap_remove(me->pendreqs, &r->blk);

        assert(me->func->peer_giveback_block);
        me->func->peer_giveback_block(me->caller, me->peer_udata, &r->blk);
        free(r);
    }
    llqueue_free(rem);
#endif

}

void pwp_conn_release(pwp_conn_t* me_)
{
    pwp_conn_private_t *me = (void*)me_;

    __expunge_their_pending_reqs(me);
    __expunge_my_pending_reqs(me);
    hashmap_free(me->pendreqs);
    llqueue_free(me->pendpeerreqs);
    free(me_);
}

void pwp_conn_set_piece_info(pwp_conn_t* me_, int num_pieces, int piece_len)
{
    pwp_conn_private_t *me = (void*)me_;

    me->num_pieces = num_pieces;
    bitfield_init(&me->state.have_bitfield, me->num_pieces);
    me->piece_len = piece_len;
}

void pwp_conn_set_functions(pwp_conn_t* me_, pwp_conn_functions_t* funcs, void* caller)
{
    pwp_conn_private_t *me = (void*)me_;

    me->func = funcs;
    me->caller = caller;
}

int pwp_conn_peer_is_interested(pwp_conn_t* me_)
{
    pwp_conn_private_t *me = (void*)me_;

    return 0 != (me->state.flags & PC_PEER_INTERESTED);
}

int pwp_conn_peer_is_choked(pwp_conn_t* me_)
{
    pwp_conn_private_t *me = (void*)me_;

    return 0 != (me->state.flags & PC_IM_CHOKING);
}

/**
 *
 */
int pwp_conn_flag_is_set(pwp_conn_t* me_, const int flag)
{
    pwp_conn_private_t *me = (void*)me_;

    return 0 != (me->state.flags & flag);
}

/**
 * @return whether I am choked or not
 */
int pwp_conn_im_choked(pwp_conn_t* me_)
{
    pwp_conn_private_t *me = (void*)me_;

    return 0 != (me->state.flags & PC_PEER_CHOKING);
}

int pwp_conn_im_interested(pwp_conn_t* me_)
{
    pwp_conn_private_t *me = (void*)me_;

    return 0 != (me->state.flags & PC_IM_INTERESTED);
}

void pwp_conn_set_im_interested(pwp_conn_t* me_)
{
    pwp_conn_private_t *me = (void*)me_;

    if (pwp_conn_send_statechange(me_, PWP_MSGTYPE_INTERESTED))
    {
        me->state.flags |= PC_IM_INTERESTED;
    }
}

void pwp_conn_choke_peer(pwp_conn_t* me_)
{
    pwp_conn_private_t *me = (void*)me_;

    me->state.flags |= PC_IM_CHOKING;
    __expunge_their_pending_reqs(me);
    pwp_conn_send_statechange(me_, PWP_MSGTYPE_CHOKE);
}

void pwp_conn_unchoke_peer(pwp_conn_t* me_)
{
    pwp_conn_private_t *me = (void*)me_;

    me->state.flags &= ~PC_IM_CHOKING;
    pwp_conn_send_statechange(me_, PWP_MSGTYPE_UNCHOKE);
}

static void *__get_piece(pwp_conn_private_t * me, const unsigned int piece_idx)
{
    assert(NULL != me->func->getpiece);
    return me->func->getpiece(me->caller, piece_idx);
}

int pwp_conn_get_download_rate(const pwp_conn_t* me_ __attribute__((__unused__)))
{
    const pwp_conn_private_t *me = (void*)me_;
    return meanqueue_get_value(me->bytes_downloaded_rate);
}

int pwp_conn_get_upload_rate(const pwp_conn_t* me_ __attribute__((__unused__)))
{
    const pwp_conn_private_t *me = (void*)me_;
    return meanqueue_get_value(me->bytes_uploaded_rate);
}

/**
 * unchoke, choke, interested, uninterested,
 * @return non-zero if unsucessful */
int pwp_conn_send_statechange(pwp_conn_t* me_, const unsigned char msg_type)
{
    pwp_conn_private_t *me = (void*)me_;
    unsigned char data[5], *ptr = data;

    bitstream_write_uint32(&ptr, fe(1));
    bitstream_write_ubyte(&ptr, msg_type);

    __log(me, "send,%s", pwp_msgtype_to_string(msg_type));

    if (!__send_to_peer(me, data, 5))
    {
        return 0;
    }

    return 1;
}

/**
 * Send the piece highlighted by this request.
 * @pararm req - the requesting block
 * */
void pwp_conn_send_piece(pwp_conn_t* me_, bt_block_t * req)
{
    pwp_conn_private_t *me = (void*)me_;
    unsigned char *data = NULL;
    unsigned char *ptr;
    void *pce;
    unsigned int size;

    assert(NULL != me);
    assert(NULL != me->func->write_block_to_stream);

    /*  get data to send */
    pce = __get_piece(me, req->piece_idx);

    /* prepare buf */
    size = 4 + 1 + 4 + 4 + req->len;
    if (!(data = malloc(size)))
    {
        perror("out of memory");
        exit(0);
    }

    ptr = data;
    bitstream_write_uint32(&ptr, fe(size - 4));
    bitstream_write_ubyte(&ptr, PWP_MSGTYPE_PIECE);
    bitstream_write_uint32(&ptr, fe(req->piece_idx));
    bitstream_write_uint32(&ptr, fe(req->offset));
    me->func->write_block_to_stream(pce,req,(unsigned char**)&ptr);
    __send_to_peer(me, data, size);

#if 0
    #define BYTES_SENT 1

    for (ii = req->len; ii > 0;)
    {
        int len = BYTES_SENT < ii ? BYTES_SENT : ii;

        bt_piece_write_block_to_str(pce,
                                    req->offset +
                                    req->len - ii, len, block);
        __send_to_peer(me, block, len);
        ii -= len;
    }
#endif

    __log(me, "send,piece,piece_idx=%d offset=%d len=%d",
          req->piece_idx, req->offset, req->len);

    free(data);
}

/**
 * Tell peer we have this piece 
 * @return 0 on error, 1 otherwise */
int pwp_conn_send_have(pwp_conn_t* me_, const int piece_idx)
{
    pwp_conn_private_t *me = (void*)me_;
    unsigned char data[12], *ptr = data;

    bitstream_write_uint32(&ptr, fe(5));
    bitstream_write_ubyte(&ptr, PWP_MSGTYPE_HAVE);
    bitstream_write_uint32(&ptr, fe(piece_idx));
    __send_to_peer(me, data, 5+4);
    __log(me, "send,have,piece_idx=%d", piece_idx);
    return 1;
}

/**
 * Send request for a block */
void pwp_conn_send_request(pwp_conn_t* me_, const bt_block_t * request)
{
    pwp_conn_private_t *me = (void*)me_;
    unsigned char data[32], *ptr;

    ptr = data;
    bitstream_write_uint32(&ptr, fe(13));
    bitstream_write_ubyte(&ptr, PWP_MSGTYPE_REQUEST);
    bitstream_write_uint32(&ptr, fe(request->piece_idx));
    bitstream_write_uint32(&ptr, fe(request->offset));
    bitstream_write_uint32(&ptr, fe(request->len));
    __send_to_peer(me, data, 13+4);
    __log(me, "send,request,piece_idx=%d offset=%d len=%d",
          request->piece_idx, request->offset, request->len);
}

/**
 * Tell peer we are cancelling the request for this block */
void pwp_conn_send_cancel(pwp_conn_t* me_, bt_block_t * cancel)
{
    pwp_conn_private_t *me = (void*)me_;
    unsigned char data[32], *ptr;

    ptr = data;
    bitstream_write_uint32(&ptr, fe(13));
    bitstream_write_ubyte(&ptr, PWP_MSGTYPE_CANCEL);
    bitstream_write_uint32(&ptr, fe(cancel->piece_idx));
    bitstream_write_uint32(&ptr, fe(cancel->offset));
    bitstream_write_uint32(&ptr, fe(cancel->len));
    __send_to_peer(me, data, 17);
    __log(me, "send,cancel,piece_idx=%d offset=%d len=%d",
          cancel->piece_idx, cancel->offset, cancel->len);
}

static void __write_bitfield_to_stream_from_getpiece_func(pwp_conn_private_t* me,
        unsigned char ** ptr)
{
    int ii;
    unsigned char bits;

    assert(NULL != me->func->getpiece);
    assert(NULL != me->func->piece_is_complete);

    /*  for all pieces set bit = 1 if we have the completed piece */
    for (bits = 0, ii = 0; ii < me->num_pieces; ii++)
    {
        void *pce;

        pce = me->func->getpiece(me->caller, ii);
        bits |= me->func->piece_is_complete(me->caller, pce) << (7 - (ii % 8));
        /* ...up to eight bits, write to byte */
        if (((ii + 1) % 8 == 0) || me->num_pieces - 1 == ii)
        {
            bitstream_write_ubyte(ptr, bits);
            bits = 0;
        }
    }
}

/**
 * Send a bitfield to peer, telling them what we have */
void pwp_conn_send_bitfield(pwp_conn_t* me_)
{
    pwp_conn_private_t *me = (void*)me_;
    unsigned char data[1000], *ptr;
    uint32_t size;

    if (!me->func->getpiece)
        return;

    ptr = data;
    size =
        sizeof(uint32_t) + sizeof(unsigned char) + (me->num_pieces / 8) +
        ((me->num_pieces % 8 == 0) ? 0 : 1);
    bitstream_write_uint32(&ptr, fe(size - sizeof(uint32_t)));
    bitstream_write_ubyte(&ptr, PWP_MSGTYPE_BITFIELD);
    __write_bitfield_to_stream_from_getpiece_func(me, &ptr);

#if 0
    /*  ensure padding */
    if (ii % 8 != 0)
    {
//        bitstream_write_ubyte(&ptr, bits);
    }
#endif

    __send_to_peer(me, data, size);
    __log(me, "send,bitfield");

}

void pwp_conn_set_state(pwp_conn_t* me_, const int state)
{
    pwp_conn_private_t *me = (void*)me_;

    me->state.flags = state;
}

int pwp_conn_get_state(pwp_conn_t* me_)
{
    pwp_conn_private_t *me = (void*)me_;

    return me->state.flags;
}

/**
 * Peer told us they have this piece.
 * @return 0 on error, 1 otherwise */
int pwp_conn_mark_peer_has_piece(pwp_conn_t* me_, const int piece_idx)
{
    pwp_conn_private_t *me = (void*)me_;
    int bf_len;

    /* make sure piece is within bitfield length */
    bf_len = bitfield_get_length(&me->state.have_bitfield);
    if (bf_len <= piece_idx || piece_idx < 0)
    {
        __disconnect(me, "piece idx fits outside of boundary");
        return 0;
    }

    /* remember that they have this piece */
    bitfield_mark(&me->state.have_bitfield, piece_idx);
    if (me->func->peer_have_piece)
        me->func->peer_have_piece(me->caller, me->peer_udata, piece_idx);

    return 1;
}

/**
 * fit the request in the piece size so that we don't break anything */
static void __request_fit(bt_block_t * request, const unsigned int piece_len)
{
    if (piece_len < request->offset + request->len)
    {
        request->len =
            request->offset + request->len - piece_len;
    }
}

/**
 * @return number of requests we required from the peer */
int pwp_conn_get_npending_requests(const pwp_conn_t* me_)
{
    const pwp_conn_private_t * me = (void*)me_;
    return hashmap_count(me->pendreqs);
}

/**
 * @return number of requests we required from the peer */
int pwp_conn_get_npending_peer_requests(const pwp_conn_t* me_)
{
    const pwp_conn_private_t * me = (void*)me_;
    return llqueue_count(me->pendpeerreqs);
}

/**
 * pend a block request */
void pwp_conn_request_block_from_peer(pwp_conn_t* me_, bt_block_t * blk)
{
    pwp_conn_private_t * me = (void*)me_;
    request_t *req;

    /*  drop meaningless blocks */
    if (blk->len < 0)
        return;

    __request_fit(blk, me->piece_len);
    pwp_conn_send_request(me_, blk);

    /* remember that we requested it */
    req = malloc(sizeof(request_t));
    req->tick = me->state.tick;
    memcpy(&req->blk, blk, sizeof(bt_block_t));
    hashmap_put(me->pendreqs, &req->blk, req);

#if 0 /*  debugging */
    printf("request block: %d %d %d",
           blk->piece_idx, blk->offset, blk->len);
#endif
}

static void __make_request(pwp_conn_private_t * me)
{
    bt_block_t blk;

    if (0 == me->func->pollblock(me->caller, me->peer_udata, &me->state.have_bitfield, &blk))
    {
        if (blk.len == 0) return;
        pwp_conn_request_block_from_peer((pwp_conn_t*)me, &blk);
    }
}

/**
 * Tells the peerconn that the connection failed */
void pwp_conn_connect_failed(pwp_conn_t* me_)
{
    pwp_conn_private_t *me = (void*)me_;

    /* check if we haven't failed before too many times
     * we do not want to stay in an end-less loop */
    me->state.failed_connections += 1;

    if (5 < me->state.failed_connections)
    {
        me->state.flags = PC_UNCONTACTABLE_PEER;
    }
    assert(0);
}

void pwp_conn_periodic(pwp_conn_t* me_)
{
    pwp_conn_private_t *me = (void*)me_;

    me->state.tick++;

    __expunge_my_old_pending_reqs(me);

    if (pwp_conn_flag_is_set(me_, PC_UNCONTACTABLE_PEER))
    {
        printf("uncontactable\n");
        goto cleanup;
    }

    /* send one pending request to the peer */
    if (0 < llqueue_count(me->pendpeerreqs))
    {
        bt_block_t* blk;

        blk = llqueue_poll(me->pendpeerreqs);
        pwp_conn_send_piece(me_, blk);
        free(blk);
    }

    /* unchoke interested peer */
    if (pwp_conn_peer_is_interested(me_))
    {
        if (pwp_conn_peer_is_choked(me_))
        {
            pwp_conn_unchoke(me_);
        }
    }

    /* request piece */
    if (pwp_conn_im_interested(me_))
    {
        int ii, end;

        if (pwp_conn_im_choked(me_))
        {
            goto cleanup;
        }

        /*  max out pipeline */
        end = 20 - pwp_conn_get_npending_requests(me_);

        for (ii = 0; ii < end; ii++)
        {
            __make_request(me);
        }
    }
    else
    {
        pwp_conn_set_im_interested(me_);
    }

#if 0 /* debugging */
    printf("pending requests: %lx %d %d\n",
            me, pwp_conn_get_npending_requests(me),
            llqueue_count(me->pendpeerreqs));
#endif

    /*  measure transfer rate */
    meanqueue_offer(me->bytes_downloaded_rate, me->bytes_downloaded_this_period);
    meanqueue_offer(me->bytes_uploaded_rate, me->bytes_uploaded_this_period);
    me->bytes_downloaded_this_period = 0;
    me->bytes_uploaded_this_period = 0;

cleanup:
    return;
}

/** 
 *  @return 1 if the peer has this piece; otherwise 0 */
int pwp_conn_peer_has_piece(pwp_conn_t* me_, const int piece_idx)
{
    pwp_conn_private_t *me = (void*)me_;

    return bitfield_is_marked(&me->state.have_bitfield, piece_idx);
}

void pwp_conn_keepalive(pwp_conn_t* me_ __attribute__((__unused__)))
{

}

void pwp_conn_choke(pwp_conn_t* me_)
{
    pwp_conn_private_t* me = (void*)me_;

    me->state.flags |= PC_PEER_CHOKING;
    __log(me, "read,choke");
    __expunge_my_pending_reqs(me);
}

void pwp_conn_unchoke(pwp_conn_t* me_)
{
    pwp_conn_private_t* me = (void*)me_;

    me->state.flags &= ~PC_PEER_CHOKING;
    __log(me, "read,unchoke");
}

void pwp_conn_interested(pwp_conn_t* me_)
{
    pwp_conn_private_t* me = (void*)me_;

    me->state.flags |= PC_PEER_INTERESTED;
    __log(me, "read,interested");
}

void pwp_conn_uninterested(pwp_conn_t* me_)
{
    pwp_conn_private_t* me = (void*)me_;

    me->state.flags &= ~PC_PEER_INTERESTED;
    __log(me, "read,uninterested");
}

void pwp_conn_have(pwp_conn_t* me_, msg_have_t* have)
{
    pwp_conn_private_t* me = (void*)me_;

//    assert(payload_len == 4);

    if (1 == pwp_conn_mark_peer_has_piece(me_, have->piece_idx))
    {
//      assert(pwp_conn_peer_has_piece(me, piece_idx));
    }

//  bitfield_mark(&me->state.have_bitfield, piece_idx);

    __log(me, "read,have,piece_idx=%d", have->piece_idx);

    /* tell the peer we are intested if we don't have this piece */
    if (!__get_piece(me, have->piece_idx))
    {
        pwp_conn_set_im_interested(me_);
    }
}

/**
 * Receive a bitfield */
void pwp_conn_bitfield(pwp_conn_t* me_, msg_bitfield_t* bitfield)
{
    pwp_conn_private_t* me = (void*)me_;
    char *str;
    int ii;

     /* A peer MUST send this message immediately after the handshake
     * operation, and MAY choose not to send it if it has no pieces at
     * all. This message MUST not be sent at any other time during the
     * communication. */

#if 0
    if (me->num_pieces < bitfield_get_length(&bitfield->bf))
    {
        __disconnect(me, "too many pieces within bitfield");
    }
#endif

    if (pwp_conn_flag_is_set(me_, PC_BITFIELD_RECEIVED))
    {
        __disconnect(me, "peer sent bitfield twice");
    }

    me->state.flags |= PC_BITFIELD_RECEIVED;

    for (ii = 0; ii < me->num_pieces; ii++)
    {
        if (bitfield_is_marked(&bitfield->bf,ii))
        {
            pwp_conn_mark_peer_has_piece(me_, ii);
        }
    }

    str = bitfield_str(&me->state.have_bitfield);
    __log(me, "read,bitfield,%s", str);
    free(str);
}

/**
 * Respond to a peer's request for a block
 * @return 0 on error, 1 otherwise */
int pwp_conn_request(pwp_conn_t* me_, bt_block_t *request)
{
    pwp_conn_private_t* me = (void*)me_;
    void *pce;

    /* check that the client doesn't request when they are choked */
    if (pwp_conn_peer_is_choked(me_))
    {
        __disconnect(me, "peer requested when they were choked");
        return 0;
    }

    /* We're choking - we aren't obligated to respond to this request */
    if (pwp_conn_peer_is_choked(me_))
    {
        return 0;
    }

    /* Ensure we have correct piece_idx */
    if (me->num_pieces < request->piece_idx)
    {
        __disconnect(me, "requested piece %d has invalid idx",
                     request->piece_idx);
        return 0;
    }

    /* Ensure that we have this piece */
    if (!(pce = __get_piece(me, request->piece_idx)))
    {
        __disconnect(me, "requested piece %d is not available",
                     request->piece_idx);
        return 0;
    }

    /* Ensure that the peer needs this piece.
     * If the peer doesn't need the piece then that means the peer is
     * potentially invalid */
    if (pwp_conn_peer_has_piece(me_, request->piece_idx))
    {
        __disconnect(me, "peer requested pce%d which they confirmed they had",
                     request->piece_idx);
        return 0;
    }

    /* Ensure that block request length is valid  */
    if (request->len == 0 || me->piece_len < request->offset + request->len)
    {
        __disconnect(me, "invalid block request"); 
        return 0;
    }

    /* Ensure that we have completed this piece.
     * The peer should know if we have completed this piece or not, so
     * asking for it is an indicator of a invalid peer. */
    assert(NULL != me->func->piece_is_complete);
    if (0 == me->func->piece_is_complete(me->caller, pce))
    {
        __disconnect(me, "requested piece %d is not completed",
                     request->piece_idx);
        return 0;
    }

    /* Append block to our pending request queue. */
    /* Don't append the block twice. */
    if (!llqueue_get_item_via_cmpfunction(
                me->pendpeerreqs,request,(void*)__request_compare))
    {
        bt_block_t* blk;

        blk = malloc(sizeof(bt_block_t));
        memcpy(blk,request, sizeof(bt_block_t));
        llqueue_offer(me->pendpeerreqs,blk);
    }

    return 1;
}

/**
 * Receive a cancel message. */
void pwp_conn_cancel(pwp_conn_t* me_, bt_block_t *cancel)
{
    pwp_conn_private_t* me = (void*)me_;
    bt_block_t *removed;

    __log(me, "read,cancel,piece_idx=%d offset=%d length=%d",
          cancel->piece_idx, cancel->offset,
          cancel->len);

    removed = llqueue_remove_item_via_cmpfunction(
            me->pendpeerreqs, cancel, (void*)__request_compare);

    free(removed);

//  queue_remove(peer->request_queue);
}

/**
 * @return 1 if the request is still pending; otherwise 0 */
int pwp_conn_block_request_is_pending(void* pc, bt_block_t *b)
{
    pwp_conn_private_t* me = pc;
    return NULL != hashmap_get(me->pendreqs, b);
}

static void pwp_conn_remove_pending_request(pwp_conn_private_t* me, const msg_piece_t *p)
{
    request_t *r;
    void *add;

    /* remove pending request */
    if ((r = hashmap_remove(me->pendreqs, &p->blk)))
    {
        free(r);
        return;
    }

    add = llqueue_new();

#if 0
        /* ensure that the peer is sending us a piece we requested */
        __disconnect(me, "err: received a block we did not request: %d %d %d\n",
                     piece->block.piece_idx,
                     piece->block.offset,
                     piece->block.len);
        return 0;
#endif

    hashmap_iterator_t iter;

    /*  find out if this piece is part of another request */
    for (hashmap_iterator(me->pendreqs, &iter);
         (r = hashmap_iterator_next_value(me->pendreqs, &iter));)
    {
        llqueue_offer(add,r);
    }

//    for (hashmap_iterator(me->pendreqs, &iter);
//         (r = hashmap_iterator_next_value(me->pendreqs, &iter));)

    /*  find out if this piece is part of another request */
    while (llqueue_count(add) > 0)
    {
        const bt_block_t *pb;
        bt_block_t *rb;

        r = llqueue_poll(add);

        rb = &r->blk;
        pb = &p->blk;

        if (r->blk.piece_idx != pb->piece_idx) continue;

        /*  piece completely eats request */
        if (pb->offset <= rb->offset &&
            rb->offset + rb->len <= pb->offset + pb->len)
        {
            r = hashmap_remove(me->pendreqs, &r->blk);
            assert(r);
            free(r);
        }
        /*
         * Piece in the middle
         * |00000LXL00000|
         */
        else if (rb->offset < pb->offset &&
                pb->offset + pb->len < rb->offset + rb->len)
        {
            request_t *n;

            n = malloc(sizeof(request_t));
            n->tick = r->tick;
            n->blk.piece_idx = rb->piece_idx;
            n->blk.offset = pb->offset + pb->len;
            n->blk.len = rb->len - pb->len - (pb->offset - rb->offset);
            assert((int)n->blk.len != 0);
            assert((int)n->blk.len > 0);
//            llqueue_offer(add,n);
            hashmap_put(me->pendreqs, &n->blk, n);
            assert(n->blk.len > 0);

            hashmap_remove(me->pendreqs, &r->blk);
            rb->len = pb->offset - rb->offset;
            assert((int)rb->len > 0);
            //llqueue_offer(add,r);
            hashmap_put(me->pendreqs, &r->blk, r);
            assert(rb->len > 0);
        }
        /*  piece splits it on the left side */
        else if (rb->offset < pb->offset + pb->len &&
            pb->offset + pb->len < rb->offset + rb->len)
        {
            hashmap_remove(me->pendreqs, &r->blk);

            /*  resize and return to hashmap */
            rb->len -= (pb->offset + pb->len) - rb->offset;
            rb->offset = pb->offset + pb->len;
            assert((int)rb->len > 0);

            hashmap_put(me->pendreqs, &r->blk, r);
            //llqueue_offer(add,r);
        }
        /*  piece splits it on the right side */
        else if (rb->offset < pb->offset &&
            pb->offset < rb->offset + rb->len &&
            rb->offset + rb->len <= pb->offset + pb->len)
        {
            hashmap_remove(me->pendreqs, &r->blk);

            /*  resize and return to hashmap */
            rb->len = pb->offset - rb->offset;
            assert((int)rb->len > 0);
            
            hashmap_put(me->pendreqs, &r->blk, r);
            //llqueue_offer(add,r);
        }
    }

#if 0
    while (llqueue_count(add) > 0)
    {
        r = llqueue_poll(add);
        hashmap_put(me->pendreqs, &r->blk, r);
    }
#endif

    llqueue_free(add);
}

/**
 * Receive a piece message
 * @return 1; otherwise 0 on failure */
int pwp_conn_piece(pwp_conn_t* me_, msg_piece_t *p)
{
    pwp_conn_private_t* me = (void*)me_;

    assert(me->func->pushblock);
    __log(me, "READ,piece,piece_idx=%d offset=%d length=%d",
          p->blk.piece_idx,
          p->blk.offset,
          p->blk.len);

    pwp_conn_remove_pending_request(me,p);

    me->func->pushblock(
            me->caller,
            me->peer_udata,
            &p->blk,
            p->data);

    me->bytes_downloaded_this_period += p->blk.len;
    return 1;
}
