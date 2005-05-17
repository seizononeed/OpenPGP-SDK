#ifndef OPS_MEMORY_H
#define OPS_MEMORY_H

#include <sys/types.h>
#include <openssl/bn.h>
#include "packet.h"

typedef struct 
    {
    unsigned char *buf;
    size_t length;
    size_t allocated;
    } ops_memory_t;

void ops_memory_init(ops_memory_t *mem,size_t initial_size);
void ops_memory_pad(ops_memory_t *mem,size_t length);
void ops_memory_add(ops_memory_t *mem,const unsigned char *src,size_t length);
void ops_memory_add_int(ops_memory_t *mem,unsigned n,size_t length);
void ops_memory_add_mpi(ops_memory_t *out,const BIGNUM *bn);
void ops_memory_make_packet(ops_memory_t *out,ops_content_tag_t tag);
void ops_memory_release(ops_memory_t *mem);

#endif