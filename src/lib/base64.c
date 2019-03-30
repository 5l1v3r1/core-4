/* Copyright (c) 2007-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "base64.h"
#include "buffer.h"

/*
 * Low-level Base64 encoder
 */

static size_t
base64_encode_get_out_size(struct base64_encoder *enc, size_t src_size)
{
	size_t res_size = enc->w_buf_len;

	i_assert(enc->w_buf_len <= 4);

	if (src_size == 0)
		return res_size;

	/* Handle sub-position */
	switch (enc->sub_pos) {
	case 0:
		break;
	case 1:
		res_size++;
		src_size--;
		if (src_size == 0)
			return res_size;
		/* fall through */
	case 2:
		res_size += 2;
		src_size--;
		break;
	default:
		i_unreached();
	}

	/* We're now at a 3-byte boundary */
	if (src_size == 0)
		return res_size;

	/* Calculate size we can append to the output from remaining input */
	res_size += ((src_size) / 3) * 4;
	switch (src_size % 3) {
	case 0:
		break;
	case 1:
		res_size += 1;
		break;
	case 2:
		res_size += 2;
		break;
	}
	return res_size;
}

size_t base64_encode_get_size(struct base64_encoder *enc, size_t src_size)
{
	size_t out_size = base64_encode_get_out_size(enc, src_size);

	if (src_size == 0) {
		/* last block */
		switch (enc->sub_pos) {
		case 0:
			break;
		case 1:
			out_size += 3;
			break;
		case 2:
			out_size += 2;
			break;
		default:
			i_unreached();
		}
	}

	return out_size;
}

static void
base64_encode_more_data(struct base64_encoder *enc,
			const unsigned char *src_c, size_t src_size,
			size_t *src_pos_r, size_t dst_avail, buffer_t *dest)
{
	const struct base64_scheme *b64 = enc->b64;
	const char *b64enc = b64->encmap;
	size_t res_size;
	unsigned char *start, *ptr, *end;
	size_t src_pos;

	/* determine how much we can write in destination buffer */
	if (dst_avail == 0) {
		*src_pos_r = 0;
		return;
	}

	/* pre-allocate space in the destination buffer */
	res_size = base64_encode_get_out_size(enc, src_size);
	if (res_size > dst_avail)
		res_size = dst_avail;

	start = buffer_append_space_unsafe(dest, res_size);
	end = start + res_size;
	ptr = start;

	/* write bytes not written in previous call */
	i_assert(enc->w_buf_len <= 4);
	if (enc->w_buf_len > res_size) {
		memcpy(ptr, enc->w_buf, res_size);
		ptr += res_size;
		enc->w_buf_len -= res_size;
		memmove(enc->w_buf, enc->w_buf + res_size, enc->w_buf_len);
	} else if (enc->w_buf_len > 0) {
		memcpy(ptr, enc->w_buf, enc->w_buf_len);
		ptr += enc->w_buf_len;
		enc->w_buf_len = 0;
	}
	if (ptr == end) {
		*src_pos_r = 0;
		return;
	}
	i_assert(enc->w_buf_len == 0);
	i_assert(src_size != 0);

	/* Handle sub-position */
	src_pos = 0;
	switch (enc->sub_pos) {
	case 0:
		break;
	case 1:
		i_assert(ptr < end);
		ptr[0] = b64enc[enc->buf | (src_c[src_pos] >> 4)];
		ptr++;
		enc->buf = (src_c[src_pos] & 0x0f) << 2;
		src_pos++;
		if (src_pos == src_size || ptr == end) {
			enc->sub_pos = 2;
			*src_pos_r = src_pos;
			return;
		}
		/* fall through */
	case 2:
		ptr[0] = b64enc[enc->buf | ((src_c[src_pos] & 0xc0) >> 6)];
		enc->w_buf[0] = b64enc[src_c[src_pos] & 0x3f];
		ptr++;
		src_pos++;
		if (ptr < end) {
			ptr[0] = enc->w_buf[0];
			ptr++;
			enc->w_buf_len = 0;
		} else {
			enc->sub_pos = 0;
			enc->w_buf_len = 1;
			*src_pos_r = src_pos;
			return;
		}
		break;
	default:
		i_unreached();
	}
	enc->sub_pos = 0;

	/* We're now at a 3-byte boundary */
	if (src_pos == src_size) {
		i_assert(ptr == end);
		*src_pos_r = src_pos;
		return;
	}

	/* Convert the bulk */
	for (; src_size - src_pos > 2 && &ptr[3] < end;
	     src_pos += 3, ptr += 4) {
		ptr[0] = b64enc[src_c[src_pos] >> 2];
		ptr[1] = b64enc[((src_c[src_pos] & 0x03) << 4) |
				(src_c[src_pos+1] >> 4)];
		ptr[2] = b64enc[((src_c[src_pos+1] & 0x0f) << 2) |
				((src_c[src_pos+2] & 0xc0) >> 6)];
		ptr[3] = b64enc[src_c[src_pos+2] & 0x3f];
	}

	/* Convert the bytes beyond the last 3-byte boundary and update state
	   for next call */
	switch (src_size - src_pos) {
	case 0:
		enc->sub_pos = 0;
		enc->buf = 0;
		break;
	case 1:
		enc->sub_pos = 1;
		enc->w_buf[0] = b64enc[src_c[src_pos] >> 2];
		enc->w_buf_len = 1;
		enc->buf = (src_c[src_pos] & 0x03) << 4;
		src_pos++;
		break;
	case 2:
		enc->sub_pos = 2;
		enc->w_buf[0] = b64enc[src_c[src_pos] >> 2];
		enc->w_buf[1] = b64enc[((src_c[src_pos] & 0x03) << 4) |
				       (src_c[src_pos+1] >> 4)];
		enc->w_buf_len = 2;
		enc->buf = (src_c[src_pos+1] & 0x0f) << 2;
		src_pos += 2;
		res_size = end - ptr;
		break;
	default:
		/* hit the end of the destination buffer */
		enc->sub_pos = 0;
		enc->w_buf[0] = b64enc[src_c[src_pos] >> 2];
		enc->w_buf[1] = b64enc[((src_c[src_pos] & 0x03) << 4) |
				       (src_c[src_pos+1] >> 4)];
		enc->w_buf[2] = b64enc[((src_c[src_pos+1] & 0x0f) << 2) |
				       ((src_c[src_pos+2] & 0xc0) >> 6)];
		enc->w_buf[3] = b64enc[src_c[src_pos+2] & 0x3f];
		enc->w_buf_len = 4;
		enc->buf = 0;
		src_pos += 3;
	}

	/* fill the remaining allocated space */
	i_assert(ptr <= end);
	res_size = end - ptr;
	i_assert(enc->w_buf_len <= 4);
	if (enc->w_buf_len > res_size) {
		memcpy(ptr, enc->w_buf, res_size);
		ptr += res_size;
		enc->w_buf_len -= res_size;
		memmove(enc->w_buf, enc->w_buf + res_size, enc->w_buf_len);
	} else if (enc->w_buf_len > 0) {
		memcpy(ptr, enc->w_buf, enc->w_buf_len);
		ptr += enc->w_buf_len;
		enc->w_buf_len = 0;
	}

	i_assert(ptr == end);
	*src_pos_r = src_pos;
}

bool base64_encode_more(struct base64_encoder *enc,
			const void *src, size_t src_size, size_t *src_pos_r,
			buffer_t *dest)
{
	const unsigned char *src_c = src;
	size_t src_pos, dst_avail;

	i_assert(!enc->finished);

	/* determine how much we can write in destination buffer */
	dst_avail = buffer_get_avail_size(dest);
	if (dst_avail == 0) {
		i_assert(src_pos_r != NULL);
		*src_pos_r = 0;
		return FALSE;
	}

	base64_encode_more_data(enc, src_c, src_size, &src_pos,
				dst_avail, dest);

	if (src_pos_r != NULL)
		*src_pos_r = src_pos;
	return (src_pos == src_size);
}

bool base64_encode_finish(struct base64_encoder *enc, buffer_t *dest)
{
	const struct base64_scheme *b64 = enc->b64;
	const char *b64enc = b64->encmap;
	size_t dst_avail;
	unsigned char w_buf[7];
	unsigned int w_buf_len = 0;

	dst_avail = 0;
	if (dest != NULL)
		dst_avail = buffer_get_avail_size(dest);

	i_assert(!enc->finished);

	if (enc->w_buf_len > 0) {
		if (dst_avail == 0)
			return FALSE;
		i_assert(enc->w_buf_len <= 4);
		memcpy(w_buf, enc->w_buf, enc->w_buf_len);
		w_buf_len += enc->w_buf_len;
	}

	switch (enc->sub_pos) {
	case 0:
		break;
	case 1:
		w_buf[w_buf_len + 0] = b64enc[enc->buf];
		w_buf[w_buf_len + 1] =  '=';
		w_buf[w_buf_len + 2] =  '=';
		w_buf_len += 3;
		break;
	case 2:
		w_buf[w_buf_len + 0] = b64enc[enc->buf];
		w_buf[w_buf_len + 1] =  '=';
		w_buf_len += 2;
		break;
	default:
		i_unreached();
	}
	enc->sub_pos = 0;

	if (w_buf_len == 0) {
		enc->finished = TRUE;
		return TRUE;
	}

	i_assert(dest != NULL);
	if (dst_avail < w_buf_len)
		return FALSE;

	buffer_append(dest, w_buf, w_buf_len);
	enc->finished = TRUE;
	return TRUE;
}

/*
 * Low-level Base64 decoder
 */

#define IS_EMPTY(c) \
	((c) == '\n' || (c) == '\r' || (c) == ' ' || (c) == '\t')

static inline void
base64_skip_whitespace(struct base64_decoder *dec ATTR_UNUSED,
		       const unsigned char *src_c,
		       size_t src_size, size_t *src_pos)
{
	/* skip any whitespace in the padding */
	while ((*src_pos) < src_size && IS_EMPTY(src_c[(*src_pos)]))
		(*src_pos)++;
}

int base64_decode_more(struct base64_decoder *dec,
		       const void *src, size_t src_size, size_t *src_pos_r,
		       buffer_t *dest)
{
	const struct base64_scheme *b64 = dec->b64;
	const unsigned char *src_c = src;
	bool expect_boundary = HAS_ALL_BITS(dec->flags,
					    BASE64_DECODE_FLAG_EXPECT_BOUNDARY);
	size_t src_pos, dst_avail;
	int ret = 1;

	i_assert(!dec->finished);
	i_assert(!dec->failed);

	if (dec->seen_boundary) {
		/* already seen the boundary/end of base64 data */
		if (src_pos_r != NULL)
			*src_pos_r = 0;
		dec->failed = TRUE;
		return -1;
	}

	src_pos = 0;
	if (dec->seen_end) {
		/* skip any whitespace at the end */
		base64_skip_whitespace(dec, src_c, src_size, &src_pos);
		if (src_pos_r != NULL)
			*src_pos_r = src_pos;
		if (src_pos < src_size) {
			if (!expect_boundary) {
				dec->failed = TRUE;
				return -1;
			}
			dec->seen_boundary = TRUE;
			return 0;
		}
		/* more whitespace may follow */
		return 1;
	}

	if (src_size == 0) {
		if (src_pos_r != NULL)
			*src_pos_r = 0;
		return 1;
	}

	dst_avail = buffer_get_avail_size(dest);
	if (dst_avail == 0) {
		i_assert(src_pos_r != NULL);
		*src_pos_r = 0;
		return 1;
	}

	for (; !dec->seen_padding && src_pos < src_size; src_pos++) {
		unsigned char in = src_c[src_pos];
		unsigned char dm = b64->decmap[in];

		if (dm == 0xff) {
			if (unlikely(!IS_EMPTY(in))) {
				ret = -1;
				break;
			}
			continue;
		}

		switch (dec->sub_pos) {
		case 0:
			dec->buf = dm;
			dec->sub_pos++;
			break;
		case 1:
			dec->buf = (dec->buf << 2) | (dm >> 4);
			buffer_append_c(dest, dec->buf);
			dst_avail--;
			dec->buf = dm;
			dec->sub_pos++;
			break;
		case 2:
			dec->buf = (dec->buf << 4) | (dm >> 2);
			buffer_append_c(dest, dec->buf);
			dst_avail--;
			dec->buf = dm;
			dec->sub_pos++;
			break;
		case 3:
			dec->buf = ((dec->buf << 6) & 0xc0) | dm;
			buffer_append_c(dest, dec->buf);
			dst_avail--;
			dec->buf = 0;
			dec->sub_pos = 0;
			break;
		default:
			i_unreached();
		}
		if (dst_avail == 0) {
			i_assert(src_pos_r != NULL);
			*src_pos_r = src_pos + 1;
			return 1;
		}
	}

	if (dec->seen_padding) {
		/* skip any whitespace in or after the padding */
		base64_skip_whitespace(dec, src_c, src_size, &src_pos);
		if (src_pos == src_size) {
			if (src_pos_r != NULL)
				*src_pos_r = src_pos;
			return 1;
		}
	}

	if (dec->seen_padding || ret < 0) {
		/* try to parse the end (padding) of the base64 input */
		i_assert(src_pos < src_size);

		switch (dec->sub_pos) {
		case 0:
		case 1:
			/* no padding expected */
			ret = -1;
			break;
		case 2:
			if (unlikely(src_c[src_pos] != '=')) {
				/* invalid character */
				ret = -1;
				break;
			}
			dec->seen_padding = TRUE;
			dec->sub_pos++;
			src_pos++;
			if (src_pos == src_size) {
				ret = 1;
				break;
			}
			/* skip any whitespace in the padding */
			base64_skip_whitespace(dec, src_c, src_size,
					       &src_pos);
			if (src_pos == src_size) {
				ret = 1;
				break;
			}
			/* fall through */
		case 3:
			if (unlikely(src_c[src_pos] != '=')) {
				/* invalid character */
				ret = -1;
				break;
			}
			dec->seen_padding = TRUE;
			dec->seen_end = TRUE;
			dec->sub_pos = 0;
			src_pos++;
			/* skip any trailing whitespace */
			base64_skip_whitespace(dec, src_c, src_size,
					       &src_pos);
			if (src_pos < src_size) {
				ret = -1;
				break;
			}
			/* more whitespace may follow */
			ret = 1;
			break;
		}
	}

	if (ret < 0) {
		if (!expect_boundary) {
			dec->failed = TRUE;
		} else {
			dec->seen_boundary = TRUE;
			ret = 0;
		}
	}
	if (src_pos_r != NULL)
		*src_pos_r = src_pos;
	return ret;
}

int base64_decode_finish(struct base64_decoder *dec)
{
	i_assert(!dec->finished);
	dec->finished = TRUE;

	return (!dec->failed && dec->sub_pos == 0 ? 0 : -1);
}

/*
 * Generic Base64 API
 */

int base64_scheme_decode(const struct base64_scheme *b64,
			 const void *src, size_t src_size, size_t *src_pos_r,
			 buffer_t *dest)
{
	const unsigned char *b64dec = b64->decmap;
	const unsigned char *src_c = src;
	size_t src_pos;
	unsigned char input[4], output[3];
	int ret = 1;

	for (src_pos = 0; src_pos+3 < src_size; ) {
		input[0] = b64dec[src_c[src_pos]];
		if (input[0] == 0xff) {
			if (unlikely(!IS_EMPTY(src_c[src_pos]))) {
				ret = -1;
				break;
			}
			src_pos++;
			continue;
		}

		input[1] = b64dec[src_c[src_pos+1]];
		if (unlikely(input[1] == 0xff)) {
			ret = -1;
			break;
		}
		output[0] = (input[0] << 2) | (input[1] >> 4);

		input[2] = b64dec[src_c[src_pos+2]];
		if (input[2] == 0xff) {
			if (unlikely(src_c[src_pos+2] != '=' ||
				     src_c[src_pos+3] != '=')) {
				ret = -1;
				break;
			}
			buffer_append(dest, output, 1);
			ret = 0;
			src_pos += 4;
			break;
		}

		output[1] = (input[1] << 4) | (input[2] >> 2);
		input[3] = b64dec[src_c[src_pos+3]];
		if (input[3] == 0xff) {
			if (unlikely(src_c[src_pos+3] != '=')) {
				ret = -1;
				break;
			}
			buffer_append(dest, output, 2);
			ret = 0;
			src_pos += 4;
			break;
		}

		output[2] = ((input[2] << 6) & 0xc0) | input[3];
		buffer_append(dest, output, 3);
		src_pos += 4;
	}

	for (; src_pos < src_size; src_pos++) {
		if (!IS_EMPTY(src_c[src_pos]))
			break;
	}

	if (src_pos_r != NULL)
		*src_pos_r = src_pos;

	return ret;
}

buffer_t *t_base64_scheme_decode_str(const struct base64_scheme *b64,
				     const char *str)
{
	buffer_t *buf;
	size_t len = strlen(str);

	buf = t_buffer_create(MAX_BASE64_DECODED_SIZE(len));
	(void)base64_scheme_decode(b64, str, len, NULL, buf);
	return buf;
}

/*
 * "base64" encoding scheme (RFC 4648, Section 4)
 */

struct base64_scheme base64_scheme = {
	.encmap = {
		'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
		'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
		'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
		'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
		'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
		'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
		'w', 'x', 'y', 'z', '0', '1', '2', '3',
		'4', '5', '6', '7', '8', '9', '+', '/',
	},
	.decmap = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 0-7 */
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 8-15 */
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 16-23 */
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 24-31 */
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 32-39 */
		0xff, 0xff, 0xff, 0x3e, 0xff, 0xff, 0xff, 0x3f, /* 40-47 */
		0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, /* 48-55 */
		0x3c, 0x3d, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 56-63 */
		0xff, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, /* 64-71 */
		0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, /* 72-79 */
		0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, /* 80-87 */
		0x17, 0x18, 0x19, 0xff, 0xff, 0xff, 0xff, 0xff, /* 88-95 */
		0xff, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, /* 96-103 */
		0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, /* 104-111 */
		0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, /* 112-119 */
		0x31, 0x32, 0x33, 0xff, 0xff, 0xff, 0xff, 0xff, /* 120-127 */

		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 128-255 */
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	},
};

/*
 * "base64url" encoding scheme (RFC 4648, Section 5)
 */

struct base64_scheme base64url_scheme = {
	.encmap = {
		'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
		'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
		'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
		'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
		'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
		'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
		'w', 'x', 'y', 'z', '0', '1', '2', '3',
		'4', '5', '6', '7', '8', '9', '-', '_',
	},
	.decmap = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 0-7 */
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 8-15 */
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 16-23 */
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 24-31 */
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 32-39 */
		0xff, 0xff, 0xff, 0xff, 0xff, 0x3e, 0xff, 0xff, /* 40-47 */
		0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, /* 48-55 */
		0x3c, 0x3d, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 56-63 */
		0xff, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, /* 64-71 */
		0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, /* 72-79 */
		0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, /* 80-87 */
		0x17, 0x18, 0x19, 0xff, 0xff, 0xff, 0xff, 0x3f, /* 88-95 */
		0xff, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, /* 96-103 */
		0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, /* 104-111 */
		0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, /* 112-119 */
		0x31, 0x32, 0x33, 0xff, 0xff, 0xff, 0xff, 0xff, /* 120-127 */

		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 128-255 */
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	},
};
