#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include <stdbool.h>
#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "CuTest.h"

#include "bitfield.h"
#include "pwp_connection.h"
#include "bitstream.h"
#include "mock_block_readwriter.h"
#include "mock_piece.h"
#include "bt_diskmem.h"
#include "test_connection.h"
#include "chunkybar.h"

int __FUNC_peercon_recv(
    void* r,
    void * peer __attribute__((__unused__)),
    char *buf,
    int *len
)
{
    test_sender_t * sender = r;

    memcpy(buf, &sender->read_data[sender->read_pos], sizeof(char) * (*len));
    sender->read_pos += *len;

#if 0
    printf("read:");
    int ii;

    for (ii = 0; ii < *len; ii++)
    {
        printf("%d ", buf[ii]);
    }
    printf("\n");
#endif

    return 1;
}

/*----------------------------------------------------------------------------*/

int __FUNC_connect(
    /* caller */
    void* r __attribute__((__unused__)), 
    /* me */
    void* me __attribute__((__unused__)),
    void * peer __attribute__((__unused__))
)
{
//    test_sender_t * sender = r;
    return 1;
}

int __FUNC_disconnect(
    void* r,
    void * peer __attribute__((__unused__)),
    char *reason __attribute__((__unused__))
)
{
    test_sender_t * sender = r;
    sender->has_disconnected = 1;
//    printf("DISCONNECT: %s\n", reason);
    return 0;
}

int __FUNC_MOCK_push_block(
    void * sender __attribute__((__unused__)), 
    void * peer  __attribute__((__unused__)),
    bt_block_t * block __attribute__((__unused__)),
    const void *data __attribute__((__unused__))
)
{
    return 1;
}

/*  allocate the block */
int __FUNC_push_block(
        void* r,
    void * peer __attribute__((__unused__)),
    bt_block_t * block,
    const void *data
)
{
    test_sender_t * sender = r;

    memcpy(&sender->read_last_block, block, sizeof(bt_block_t));
    assert(block->len < 10);
    memcpy(sender->read_last_block_data, data, sizeof(char) * block->len);
    return 1;
}

/*----------------------------------------------------------------------------*/
/*  Send data                                                                 */
/*----------------------------------------------------------------------------*/

char *__sender_set(
    test_sender_t * sender,
    char *read_msg,
    char *send_msg
)
{
    bt_block_t blk;
    char piecedata[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    void *dc;

    /* init */
    memset(sender, 0, sizeof(test_sender_t));

    /* send */
    sender->send_pos = 0;
    sender->send_data = (char*)send_msg;

    /* read */
    sender->read_pos = 0;
    sender->read_data = (char*)read_msg;
    sender->has_disconnected = 0;

    /* create holding place for data written */
    dc = bt_diskmem_new();
    bt_diskmem_set_size(dc, 4);

    /* create mock piece */
    sender->piece = mock_piece_new("00000000000000000000", 4);
    mock_piece_set_disk_blockrw(sender->piece, bt_diskmem_get_blockrw(dc), dc);
    /* write block of length 4 */
    /* piece idx not necessary */
    blk.piece_idx = 0;
    blk.offset = 0;
    blk.len = 4;
    mock_piece_write_block(sender->piece, NULL, &blk, piecedata);

    return NULL;
}

void __FUNC_peer_piece_have(
    void *udata,
    void *peer __attribute__((__unused__)),
    int piece
)
{
    test_sender_t * sender = udata;
    chunkybar_t* sc = sender->sc;
    chunky_mark_complete(sc, piece, 1);
}

/*  Just a mock.
 *  Used when sent data is not tested */
int __FUNC_MOCK_send(
    void* s __attribute__((__unused__)),
    const void *peer __attribute__((__unused__)),
    const void *send_data __attribute__((__unused__)),
    const int len __attribute__((__unused__))
)
{
    return 1;
}

int __FUNC_send(
    void* s,
    const void *peer __attribute__((__unused__)),
    const void *send_data,
    const int len
)
{
    test_sender_t * sender = s;

#if 0
    int ii;

    for (ii = 0; ii < len; ii++)
    {
        if (ii % 4 == 0 && ii != 0) printf(" ");
        printf("%02x,",// %c,",
                ((const char*)send_data)[ii]
               //,((const char*)send_data)[ii]
                );
    }
    printf("\n");
#endif

    memcpy(sender->send_data + sender->send_pos, send_data, len);
    sender->send_pos += len;
    sender->nsent_messages += 1;
    return 1;
}

int __FUNC_failing_send(
    void * sender __attribute__((__unused__)),
    const void *peer __attribute__((__unused__)),
    const void *send_data __attribute__((__unused__)),
    const int len __attribute__((__unused__))
)
{
    return 0;
}

void *__FUNC_sender_get_piece(
    void* s,
    const unsigned int idx __attribute__((__unused__))
)
{
    test_sender_t * sender = s;
    void *piece;

    piece = sender->piece;
    return piece;
}

void* __FUNC_get_piece_never_have(
    void* s __attribute__((__unused__)),
    const unsigned int idx __attribute__((__unused__))
)
{
    return NULL;
}

/**
 * Mock an always complete piece */
int __FUNC_pieceiscomplete(
    void *bto __attribute__((__unused__)),
    void *piece __attribute__((__unused__))
)
{
//    void *piece;
//    piece = sender->piece;
    return 1;
}

/**
 * this function pretends as every piece is incomplete
 */
int __FUNC_pieceiscomplete_fail(
    void *bto __attribute__((__unused__)),
    void *piece __attribute__((__unused__))
)
{
    return 0;
}

void __FUNC_piece_write_block_to_stream(
    void * me,
    bt_block_t * blk,
    char ** msg
)
{
    void *pce;

    pce = __FUNC_sender_get_piece(me,blk->piece_idx);
    mock_piece_write_block_to_stream(pce,blk,msg);
}

