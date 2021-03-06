/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */

/**
 * @copyright 2013 Couchbase, Inc.
 *
 * @author Filipe Manana  <filipe@couchbase.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy of
 * the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations under
 * the License.
 **/

#include "index_header.h"
#include "../bitfield.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <snappy-c.h>


#define BITMASK_BYTE_SIZE      (1024 / CHAR_BIT)

#define dec_uint16(b) (decode_raw16(*((raw_16 *) b)))
#define dec_uint48(b) (decode_raw48(*((raw_48 *) b)))

static void enc_uint16(uint16_t u, char **buf);
static void enc_uint48(uint64_t u, char **buf);
static void enc_seq_list(const void *list, char **buf);
static void enc_part_seq_list(const void *list, char **buf);
static int part_seq_cmp(const void *a, const void *b);
static int part_id_cmp(const void *a, const void *b);


LIBCOUCHSTORE_API
couchstore_error_t decode_index_header(const char *bytes, size_t len, index_header_t **header)
{
    index_header_t *h = NULL;
    char *b = NULL, *uncomp = NULL;
    uint16_t num_seqs, i, sz;
    size_t uncompLen;

    /* First 16 bytes are md5 checksum (group signature). */
    if (len <= 16) {
        return COUCHSTORE_ERROR_CORRUPT;
    }

    if (snappy_uncompressed_length(bytes + 16, len, &uncompLen) != SNAPPY_OK) {
        return COUCHSTORE_ERROR_CORRUPT;
    }

    b = uncomp = (char *) malloc(uncompLen);
    if (b == NULL) {
        return COUCHSTORE_ERROR_ALLOC_FAIL;
    }

    if (snappy_uncompress(bytes + 16, len - 16, b, &uncompLen) != SNAPPY_OK) {
        goto alloc_error;
    }

    h = (index_header_t *) malloc(sizeof(index_header_t));
    if (h == NULL) {
        goto alloc_error;
    }
    h->seqs = NULL;
    h->id_btree_state = NULL;
    h->view_btree_states = NULL;
    h->replicas_on_transfer = NULL;
    h->pending_transition.active = NULL;
    h->pending_transition.passive = NULL;
    h->pending_transition.unindexable = NULL;
    h->unindexable_seqs = NULL;
    memcpy(h->signature, bytes, 16);

    h->version = (uint8_t) b[0];
    b += 1;

    h->num_partitions = dec_uint16(b);
    b += 2;

    memcpy(&h->active_bitmask, b, BITMASK_BYTE_SIZE);
    b += BITMASK_BYTE_SIZE;
    memcpy(&h->passive_bitmask, b, BITMASK_BYTE_SIZE);
    b += BITMASK_BYTE_SIZE;
    memcpy(&h->cleanup_bitmask, b, BITMASK_BYTE_SIZE);
    b += BITMASK_BYTE_SIZE;

    num_seqs = dec_uint16(b);
    b += 2;

    h->seqs = sorted_list_create(part_seq_cmp);
    if (h->seqs == NULL) {
        goto alloc_error;
    }

    for (i = 0; i < num_seqs; ++i) {
        part_seq_t pseq;

        pseq.part_id = dec_uint16(b);
        b += 2;
        pseq.seq = dec_uint48(b);
        b += 6;

        if (sorted_list_add(h->seqs, &pseq, sizeof(pseq)) != 0) {
            goto alloc_error;
        }
    }

    sz = dec_uint16(b);
    b += 2;
    h->id_btree_state = read_root((void *) b, (int) sz);
    b += sz;

    h->num_views = (uint8_t) b[0];
    b += 1;

    h->view_btree_states = (node_pointer **) malloc(sizeof(node_pointer *) * h->num_views);
    if (h->view_btree_states == NULL) {
        goto alloc_error;
    }

    for (i = 0; i < (uint16_t) h->num_views; ++i) {
        sz = dec_uint16(b);
        b += 2;
        h->view_btree_states[i] = read_root((void *) b, (int) sz);
        b += sz;
    }

    h->has_replica = b[0] == 0 ? 0 : 1;
    b += 1;

    sz = dec_uint16(b);
    b += 2;
    h->replicas_on_transfer = sorted_list_create(part_id_cmp);
    if (h->replicas_on_transfer == NULL) {
        goto alloc_error;
    }

    for (i = 0; i < sz; ++i) {
        uint16_t part_id = dec_uint16(b);
        b += 2;

        if (sorted_list_add(h->replicas_on_transfer, &part_id, sizeof(part_id)) != 0) {
            goto alloc_error;
        }
    }

    sz = dec_uint16(b);
    b += 2;

    h->pending_transition.active = sorted_list_create(part_id_cmp);
    if (h->pending_transition.active == NULL) {
        goto alloc_error;
    }

    for (i = 0; i < sz; ++i) {
        uint16_t part_id = dec_uint16(b);
        b += 2;

        if (sorted_list_add(h->pending_transition.active,
                            &part_id, sizeof(part_id)) != 0) {
            goto alloc_error;
        }
    }

    sz = dec_uint16(b);
    b += 2;

    h->pending_transition.passive = sorted_list_create(part_id_cmp);
    if (h->pending_transition.passive == NULL) {
        goto alloc_error;
    }

    for (i = 0; i < sz; ++i) {
        uint16_t part_id = dec_uint16(b);
        b += 2;

        if (sorted_list_add(h->pending_transition.passive,
                            &part_id, sizeof(part_id)) != 0) {
            goto alloc_error;
        }
    }

    sz = dec_uint16(b);
    b += 2;

    h->pending_transition.unindexable = sorted_list_create(part_id_cmp);
    if (h->pending_transition.unindexable == NULL) {
        goto alloc_error;
    }

    for (i = 0; i < sz; ++i) {
        uint16_t part_id = dec_uint16(b);
        b += 2;

        if (sorted_list_add(h->pending_transition.unindexable,
                            &part_id, sizeof(part_id)) != 0) {
            goto alloc_error;
        }
    }

    num_seqs = dec_uint16(b);
    b += 2;

    h->unindexable_seqs = sorted_list_create(part_seq_cmp);
    if (h->unindexable_seqs == NULL) {
        goto alloc_error;
    }

    for (i = 0; i < num_seqs; ++i) {
        part_seq_t pseq;

        pseq.part_id = dec_uint16(b);
        b += 2;
        pseq.seq = dec_uint48(b);
        b += 6;

        if (sorted_list_add(h->unindexable_seqs, &pseq, sizeof(pseq)) != 0) {
            goto alloc_error;
        }
    }

    free(uncomp);
    *header = h;

    return COUCHSTORE_SUCCESS;

 alloc_error:
    free_index_header(h);
    free(uncomp);
    return COUCHSTORE_ERROR_ALLOC_FAIL;
}


LIBCOUCHSTORE_API
couchstore_error_t encode_index_header(const index_header_t *header,
                                       char **buffer,
                                       size_t *buffer_size)
{
    char *buf = NULL, *b = NULL;
    size_t sz = 0;

    sz += 1;                     /* version */
    sz += 2;                     /* number of partitions */
    sz += 3 * BITMASK_BYTE_SIZE; /* active/passive/cleanup bitmasks */
    /* seqs */
    sz += 2;
    sz += sorted_list_size(header->seqs) * (2 + 6);
    /* id btree state */
    sz += 2;
    sz += sizeof(raw_btree_root);
    sz += header->id_btree_state->reduce_value.size;
    /* view btree states */
    sz += 1;
    for (int i = 0; i < header->num_views; ++i) {
        sz += 2;
        sz += sizeof(raw_btree_root);
        sz += header->view_btree_states[i]->reduce_value.size;
    }
    /* has_replicas */
    sz += 1;
    /* replicas_on_transfer */
    sz += 2;
    sz += sorted_list_size(header->replicas_on_transfer) * 2;
    /* pending transition active */
    sz += 2;
    sz += sorted_list_size(header->pending_transition.active) * 2;
    /* pending transition passive */
    sz += 2;
    sz += sorted_list_size(header->pending_transition.passive) * 2;
    /* pending transition unindexable */
    sz += 2;
    sz += sorted_list_size(header->pending_transition.unindexable) * 2;
    /* unindexable seqs */
    sz += 2;
    sz += sorted_list_size(header->unindexable_seqs) * (2 + 6);

    b = buf = (char *) malloc(sz);
    if (buf == NULL) {
        goto alloc_error;
    }

    b[0] = (char) header->version;
    b += 1;

    enc_uint16(header->num_partitions, &b);

    memcpy(b, &header->active_bitmask, BITMASK_BYTE_SIZE);
    b += BITMASK_BYTE_SIZE;
    memcpy(b, &header->passive_bitmask, BITMASK_BYTE_SIZE);
    b += BITMASK_BYTE_SIZE;
    memcpy(b, &header->cleanup_bitmask, BITMASK_BYTE_SIZE);
    b += BITMASK_BYTE_SIZE;

    enc_part_seq_list(header->seqs, &b);

    uint16_t id_btree_state_size = (uint16_t) sizeof(raw_btree_root);
    id_btree_state_size += (uint16_t) header->id_btree_state->reduce_value.size;
    enc_uint16(id_btree_state_size, &b);

    encode_root(b, header->id_btree_state);
    b += id_btree_state_size;

    b[0] = (char) header->num_views;
    b += 1;
    for (int i = 0; i < header->num_views; ++i) {
        uint16_t btree_state_size = (uint16_t) sizeof(raw_btree_root);
        btree_state_size += (uint16_t) header->view_btree_states[i]->reduce_value.size;
        enc_uint16(btree_state_size, &b);

        encode_root(b, header->view_btree_states[i]);
        b += btree_state_size;
    }

    b[0] = (char) (header->has_replica ? 1 : 0);
    b += 1;

    enc_seq_list(header->replicas_on_transfer, &b);
    enc_seq_list(header->pending_transition.active, &b);
    enc_seq_list(header->pending_transition.passive, &b);
    enc_seq_list(header->pending_transition.unindexable, &b);
    enc_part_seq_list(header->unindexable_seqs, &b);

    size_t comp_size = snappy_max_compressed_length(sz);
    char *comp = (char *) malloc(16 + comp_size);

    if (comp == NULL) {
        goto alloc_error;
    }

    snappy_status res = snappy_compress(buf, sz, comp + 16, &comp_size);

    if (res != SNAPPY_OK) {
        /* TODO: a new error for couchstore_error_t */
        free(comp);
        goto alloc_error;
    }

    memcpy(comp, header->signature, 16);
    *buffer = comp;
    *buffer_size = 16 + comp_size;
    free(buf);

    return COUCHSTORE_SUCCESS;

 alloc_error:
    free(buf);
    *buffer = NULL;
    *buffer_size = 0;
    return COUCHSTORE_ERROR_ALLOC_FAIL;
}


LIBCOUCHSTORE_API
void free_index_header(index_header_t *header)
{
    if (header == NULL) {
        return;
    }

    sorted_list_free(header->seqs);
    free(header->id_btree_state);

    if (header->view_btree_states != NULL) {
        for (int i = 0; i < header->num_views; ++i) {
            free(header->view_btree_states[i]);
        }
        free(header->view_btree_states);
    }

    sorted_list_free(header->replicas_on_transfer);
    sorted_list_free(header->pending_transition.active);
    sorted_list_free(header->pending_transition.passive);
    sorted_list_free(header->pending_transition.unindexable);
    sorted_list_free(header->unindexable_seqs);

    free(header);
}


static void enc_uint16(uint16_t u, char **buf)
{
    raw_16 r = encode_raw16(u);
    memcpy(*buf, &r, 2);
    *buf += 2;
}


static void enc_uint48(uint64_t u, char **buf)
{
    raw_48 r = encode_raw48(u);
    memcpy(*buf, &r, 6);
    *buf += 6;
}


static void enc_seq_list(const void *list, char **buf)
{
    void *it = sorted_list_iterator(list);
    uint16_t *seq = NULL;

    enc_uint16((uint16_t) sorted_list_size(list), buf);
    seq = sorted_list_next(it);
    while (seq != NULL) {
        enc_uint16(*seq, buf);
        seq = sorted_list_next(it);
    }
    sorted_list_free_iterator(it);
}


static void enc_part_seq_list(const void *list, char **buf)
{
    void *it = sorted_list_iterator(list);
    part_seq_t *pseq = NULL;

    enc_uint16((uint16_t) sorted_list_size(list), buf);
    pseq = sorted_list_next(it);
    while (pseq != NULL) {
        enc_uint16(pseq->part_id, buf);
        enc_uint48(pseq->seq, buf);
        pseq = sorted_list_next(it);
    }
    sorted_list_free_iterator(it);
}


static int part_seq_cmp(const void *a, const void *b)
{
    return ((part_seq_t *) a)->part_id - ((part_seq_t *) b)->part_id;
}


static int part_id_cmp(const void *a, const void *b)
{
    return *((uint16_t *) a) - *((uint16_t *) b);
}
