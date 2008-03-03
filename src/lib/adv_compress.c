/** \file
 */
   
#include <zlib.h>
#include <assert.h>
#include <string.h>

#include <openpgpsdk/compress.h>
#include <openpgpsdk/packet-parse.h>
#include <openpgpsdk/crypto.h>
#include <openpgpsdk/errors.h>
#include "parse_local.h"
#include <openpgpsdk/final.h>

#define DECOMPRESS_BUFFER	1024

typedef struct
    {
    ops_region_t *region;
    unsigned char in[DECOMPRESS_BUFFER];
    unsigned char out[DECOMPRESS_BUFFER];
    z_stream stream;
    size_t offset;
    int inflate_ret;
    } decompress_arg_t;

typedef struct
    {
    z_stream stream;
    unsigned char *src;
    unsigned char *dst;
    } compress_arg_t;

#define ERR(err)	do { content.content.error.error=err; content.tag=OPS_PARSER_ERROR; ops_parse_cb(&content,cbinfo); return -1; } while(0)

static int compressed_data_reader(void *dest,size_t length,
				  ops_error_t **errors,
				  ops_reader_info_t *rinfo,
				  ops_parse_cb_info_t *cbinfo)
    {
    decompress_arg_t *arg=ops_reader_get_arg(rinfo);
    ops_parser_content_t content;
    int saved=length;

    if(/*arg->region->indeterminate && */ arg->inflate_ret == Z_STREAM_END
       && arg->stream.next_out == &arg->out[arg->offset])
	return 0;

    if(arg->region->length_read == arg->region->length)
	{
	if(arg->inflate_ret != Z_STREAM_END)
	    ERR("Compressed data didn't end when region ended.");
    /*
	else
	    return 0;
    */
	}

    while(length > 0)
	{
	unsigned len;

	if(&arg->out[arg->offset] == arg->stream.next_out)
	    {
	    int ret;

	    arg->stream.next_out=arg->out;
	    arg->stream.avail_out=sizeof arg->out;
	    arg->offset=0;
	    if(arg->stream.avail_in == 0)
		{
		unsigned n=arg->region->length;

		if(!arg->region->indeterminate)
		    {
		    n-=arg->region->length_read;
		    if(n > sizeof arg->in)
			n=sizeof arg->in;
		    }
		else
		    n=sizeof arg->in;

		if(!ops_stacked_limited_read(arg->in,n,arg->region,
					     errors,rinfo,cbinfo))
		    return -1;

		arg->stream.next_in=arg->in;
		arg->stream.avail_in=arg->region->indeterminate
		    ? arg->region->last_read : n;
		}

	    ret=inflate(&arg->stream,Z_SYNC_FLUSH);
	    if(ret == Z_STREAM_END)
		{
		if(!arg->region->indeterminate
		   && arg->region->length_read != arg->region->length)
		    ERR("Compressed stream ended before packet end.");
		}
	    else if(ret != Z_OK)
		{
		fprintf(stderr,"ret=%d\n",ret);
		ERR(arg->stream.msg);
		}
	    arg->inflate_ret=ret;
	    }
	assert(arg->stream.next_out > &arg->out[arg->offset]);
	len=arg->stream.next_out-&arg->out[arg->offset];
	if(len > length)
	    len=length;
	memcpy(dest,&arg->out[arg->offset],len);
	arg->offset+=len;
	length-=len;
	}

    return saved;
    }

/**
 * \ingroup Utils
 * 
 * \param *region 	Pointer to a region
 * \param *parse_info 	How to parse
*/

int ops_decompress(ops_region_t *region,ops_parse_info_t *parse_info,
		   ops_compression_type_t type)
    {
    decompress_arg_t arg;
    int ret;

    memset(&arg,'\0',sizeof arg);

    arg.region=region;

    arg.stream.next_in=Z_NULL;
    arg.stream.avail_in=0;
    arg.stream.next_out=arg.out;
    arg.offset=0;
    arg.stream.zalloc=Z_NULL;
    arg.stream.zfree=Z_NULL;

    if(type == OPS_C_ZIP)
	ret=inflateInit2(&arg.stream,-15);
    else if(type == OPS_C_ZLIB)
	ret=inflateInit(&arg.stream);
    else if (type == OPS_C_BZIP2)
        {
        OPS_ERROR_1(&parse_info->errors, OPS_E_ALG_UNSUPPORTED_COMPRESS_ALG, "Compression algorithm %s is not yet supported", "BZIP2");
        return 0;
        }
    else
        {
        OPS_ERROR_1(&parse_info->errors, OPS_E_ALG_UNSUPPORTED_COMPRESS_ALG, "Compression algorithm %d is not yet supported", type);
        return 0;
        }

    if(ret != Z_OK)
	{
	fprintf(stderr,"ret=%d\n",ret);
	return 0;
	}

    ops_reader_push(parse_info,compressed_data_reader,NULL,&arg);

    ret=ops_parse(parse_info);

    ops_reader_pop(parse_info);

    return ret;
    }

ops_boolean_t ops_write_compressed(const unsigned char *data,
                                   const unsigned int len,
                                   ops_create_info_t *cinfo)
    {
    int r=0;
    int sz_in=0;
    int sz_out=0;
    compress_arg_t* compress=ops_mallocz(sizeof *compress);

    // compress the data
    const int level=Z_DEFAULT_COMPRESSION; // \todo allow varying levels
    compress->stream.zalloc=Z_NULL;
    compress->stream.zfree=Z_NULL;
    compress->stream.opaque=NULL;

    // all other fields set to zero by use of ops_mallocz

    if (deflateInit(&compress->stream,level) != Z_OK)
        {
        // can't initialise
        assert(0);
        }

    // do necessary transformation
    // copy input to maintain const'ness of src
    assert(compress->src==NULL);
    assert(compress->dst==NULL);

    sz_in=len * sizeof (unsigned char);
    sz_out= (sz_in * 1.01) + 12; // from zlib webpage
    compress->src=ops_mallocz(sz_in);
    compress->dst=ops_mallocz(sz_out);
    memcpy(compress->src,data,len);

    // setup stream
    compress->stream.next_in=compress->src;
    compress->stream.avail_in=sz_in;
    compress->stream.total_in=0;

    compress->stream.next_out=compress->dst;
    compress->stream.avail_out=sz_out;
    compress->stream.total_out=0;

    r=deflate(&compress->stream, Z_FINISH);
    assert(r==Z_STREAM_END); // need to loop if not

    // write it out
    return (ops_write_ptag(OPS_PTAG_CT_COMPRESSED, cinfo)
            && ops_write_length(1+compress->stream.total_out, cinfo)
            && ops_write_scalar(OPS_C_ZLIB,1,cinfo)
            && ops_write(compress->dst, compress->stream.total_out,cinfo));
    }

// EOF