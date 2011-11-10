/**
 * FreeRDP: A Remote Desktop Protocol client.
 * NSCodec Codec
 *
 * Copyright 2011 Samsung, Author Jiten Pathy
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *	 http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <freerdp/codec/nsc.h>
#include <freerdp/utils/memory.h>

/**
 * @msdn{cc241703}
 */
void nsc_cl_expand(STREAM* s, sint16* out, uint8 shiftcount, uint32 size)
{
	/*
	 * convert 9-bit 2's complement to native representation,
	 */
	while (size--)
	{
		sint16 c;

		stream_read_uint8(s, c);
		c <<= shiftcount;
		if (c & 0x100)
			c = ~0xff | c;

		*out++ = c;
	}
}

/**
 * @msdn{cc241702}
 */
void nsc_chroma_supersample(NSC_CONTEXT* context, sint16** in_h, uint32 out_size)
{
	sint16 *in, *out, *out_nextline, *buf;
	sint16 w, h, c, i;
	int pad, scan_len;

	pad = 8 - (context->width % 8);
	if (pad == 8) pad = 0;

	scan_len = context->width + pad;

	buf = xmalloc(out_size * sizeof(sint16) * 4);

	in = *in_h;
	out = buf;
	out_nextline = out + scan_len;

	for (i = 0; i < out_size / 4; i ++)
	{
		if (! (i % (scan_len/2)) && i)
		{
			out += scan_len;
			out_nextline += scan_len;
		}
		c = *in++;

		*out++ = c;
		*out++ = c;
		*out_nextline++ = c;
		*out_nextline++ = c;
	}

	xfree(*in_h);
	*in_h = buf;
}

/**
 * @msdn{cc241701}
 * @msdn{cc241700}
 */
void nsc_ycocg_combined_argb(NSC_CONTEXT* context)
{
	int i, j, p, pad;
	uint8 *bmp = context->bmpdata;

	uint8 *Y = context->org_buf[0]->data;
	sint16 *Co = context->Co;
	sint16 *Cg = context->Cg;

	sint16 a, r, g, b;

	if (context->nsc_stream->ChromaSubSamplingLevel)
	{
		pad = 8 - (context->width % 8);
		if (pad == 8) pad = 0;
	}
	else
		pad = 0;

	for (i = 0, j = 0, p = -1; i < context->OrgByteCount[3]; i++, j++, p = i % context->width)
	{
		if (!p) j += pad;

		// matrix mult
		r = Y[j] + (Co[j]/2) - (Cg[j]/2);
		g = Y[j] + (Cg[j]/2);
		b = Y[j] - (Co[j]/2) - (Cg[j]/2);

		a = context->org_buf[3]->data[i];
		if (a == 0)
			a = 0xff;

		*bmp++ = b;
		*bmp++ = g;
		*bmp++ = r;
		*bmp++ = a;
	}
}

void nsc_rle_decode(STREAM* in, STREAM* out, uint32 origsz)
{
	uint32 i;
	uint8 value;
	i = origsz;

	while (i > 4)
	{
		stream_read_uint8(in, value);

		if (i == 5)
		{
			stream_write_uint8(out,value);
			i-=1;
		}
		else if (value == *(in->p))
		{
			stream_seek(in, 1);

			if (*(in->p) < 0xFF)
			{
				uint8 len;
				stream_read_uint8(in, len);
				stream_set_byte(out, value, len+2);
				i -= (len+2);
			}
			else
			{
				uint32 len;
				stream_seek(in, 1);
				stream_read_uint32(in, len);
				stream_set_byte(out, value, len);
				i -= len;
			}
		}
		else
		{
			stream_write_uint8(out, value);
			i -= 1;
		}
	}

	stream_copy(out, in, 4);
}

void nsc_rle_decompress_data(NSC_CONTEXT* context)
{
	STREAM* rles;
	uint16 i;
	uint32 origsize;
	rles = stream_new(0);
	rles->p = rles->data = context->nsc_stream->pdata->p;
	rles->size = context->nsc_stream->pdata->size;

	for (i = 0; i < 4; i++)
	{
		origsize = context->OrgByteCount[i];

		if (i == 3 && context->nsc_stream->PlaneByteCount[3] == 0)
			stream_set_byte(context->org_buf[i], 0xff, origsize);
		else if (context->nsc_stream->PlaneByteCount[i] < origsize)
			nsc_rle_decode(rles, context->org_buf[i], origsize);
		else
			stream_copy(context->org_buf[i], rles, origsize);

		context->org_buf[i]->p = context->org_buf[i]->data;
	}
}

void nsc_stream_initialize(NSC_CONTEXT* context, STREAM* s)
{
	int i;

	for (i = 0; i < 4; i++)
		stream_read_uint32(s, context->nsc_stream->PlaneByteCount[i]);

	stream_read_uint8(s, context->nsc_stream->colorLossLevel);
	stream_read_uint8(s, context->nsc_stream->ChromaSubSamplingLevel);
	stream_seek(s, 2);

	context->nsc_stream->pdata = stream_new(0);
	stream_attach(context->nsc_stream->pdata, s->p, BYTESUM(context->nsc_stream->PlaneByteCount));
}

void nsc_context_initialize(NSC_CONTEXT* context, STREAM* s)
{
	int i;
	nsc_stream_initialize(context, s);
	context->bmpdata = xzalloc(context->width * context->height * 4);

	for (i = 0; i < 4; i++)
		context->OrgByteCount[i]=context->width * context->height;

	if (context->nsc_stream->ChromaSubSamplingLevel)	/* [MS-RDPNSC] 2.2 */
	{
		uint32 tempWidth,tempHeight;
		tempWidth = ROUND_UP_TO(context->width, 8);
		context->OrgByteCount[0] = tempWidth * context->height;

		tempHeight = ROUND_UP_TO(context->height, 2);
		tempWidth = tempWidth >> 1;
		tempHeight = tempHeight >> 1;

		context->OrgByteCount[1] = tempWidth * tempHeight;
		context->OrgByteCount[2] = tempWidth * tempHeight;
	}

	for (i = 0; i < 4; i++)
		context->org_buf[i] = stream_new(context->OrgByteCount[i]);

	context->Co = xmalloc(context->OrgByteCount[1] * sizeof(sint16));
	context->Cg = xmalloc(context->OrgByteCount[2] * sizeof(sint16));
}

void nsc_context_destroy(NSC_CONTEXT* context)
{
	int i;

	for(i = 0;i < 4; i++)
		stream_free(context->org_buf[i]);

	stream_detach(context->nsc_stream->pdata);

	xfree(context->bmpdata);
	xfree(context->Co);
	xfree(context->Cg);
}

NSC_CONTEXT* nsc_context_new(void)
{
	NSC_CONTEXT* nsc_context;
	nsc_context = xnew(NSC_CONTEXT);
	nsc_context->nsc_stream = xnew(NSC_STREAM);
	return nsc_context;
}

void nsc_context_free(NSC_CONTEXT* context)
{
	xfree(context->nsc_stream);
	xfree(context);
}

void nsc_process_message(NSC_CONTEXT* context, uint8* data, uint32 length)
{
	STREAM *s;
	s = stream_new(0);
	stream_attach(s, data, length);

	nsc_context_initialize(context, s);

	/* RLE decode */
	nsc_rle_decompress_data(context);

	/* colorloss recover */
	nsc_cl_expand(context->org_buf[1], context->Co, context->nsc_stream->colorLossLevel, context->OrgByteCount[1]);
	nsc_cl_expand(context->org_buf[2], context->Cg, context->nsc_stream->colorLossLevel, context->OrgByteCount[2]);

	/* Chroma supersample */
	if(context->nsc_stream->ChromaSubSamplingLevel)
	{
		nsc_chroma_supersample(context, &context->Co, context->OrgByteCount[0]);
		nsc_chroma_supersample(context, &context->Cg, context->OrgByteCount[0]);
	}
	
	/* YCoCg to combined ARGB */
	nsc_ycocg_combined_argb(context);
}
