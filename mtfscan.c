#include <sys/ioctl.h>
#include <sys/mtio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>

#include "mtf.h"

int mtfscan_init(struct mtf_stream *s, int fd)
{
	struct mtget mg;

	s->fd = fd;
	s->abspos = 0;

	if (ioctl(fd, MTIOCGET, &mg) == -1) {
		if (errno == ENOTTY) {
			s->blksize = 0; /* no blocks */
			errno = 0;
		} else {
			return 0;
		}
	} else {
		s->blksize = (mg.mt_dsreg & MT_ST_BLKSIZE_MASK) >> MT_ST_BLKSIZE_SHIFT;
	}

	s->ready = 0;
	s->flbsize = s->flbread = 0;

	return 1;
}

int mtfscan_ready(struct mtf_stream *s, unsigned int need)
{
	int r;

	while (s->ready < need) {
		errno = 0;
		do {
			r = read(s->fd, s->buffer + s->ready, need - s->ready);
		} while (r == -1 && errno == EINTR);
		if (r < 1) return 0;
		s->flbread += r;
		s->abspos += r;
		s->ready += r;
	}
	return 1;
}
int mtfscan_readyplus(struct mtf_stream *s, unsigned int more)
{
	return mtfscan_ready(s, s->ready + more);
}

int mtfscan_read(struct mtf_stream *s, unsigned char *buf, int chunk)
{
	int r;
	if (s->flbread + chunk > s->flbsize) {
		chunk = s->flbsize - s->flbread;
	}
	if (chunk < 1) return 0;
	errno = 0;
	do {
		r = read(s->fd, buf, chunk);
	} while (r == -1 && errno == EINTR);
	if (r < 1) return -1; /* !!! */
	s->flbread += r;
	s->abspos += r;
	return r;
}
int mtfscan_skip(struct mtf_stream *s)
{
	unsigned char stump[4096];
	int r;
	do {
		r = mtfscan_read(s, stump, sizeof(stump));
	} while (r > 0);
	return r;
}

int mtfscan_start(struct mtf_stream *s)
{
	s->flbread = 0; /* reset */
	if (!mtfscan_ready(s, 48)) return 0;
	s->header = s->buffer;
	if (mtfdb_tape_type(s)) {
		s->flbsize = mtfdb_tape_flbsize(s);
	}
	s->stringtype = mtfdb_strtype(s);
	return 1;
}
int mtfscan_next(struct mtf_stream *s)
{
	if (mtfscan_skip(s) < 0) return 0; /* skip to end of logical block */
	s->ready = 0;
	s->flbread = 0;
	return 1;
}
unsigned char *mtfscan_string(struct mtf_stream *s, struct mtf_tape_pos q, int sz)
{
	unsigned int i;
	unsigned int n;
	unsigned char *uc;
	unsigned char *out, *p;

#ifdef DEBUG
	fprintf(stderr, "q.pos=%d, q.size=%d type=%d\n", q.pos, q.size, s->stringtype);
#endif
	if (s->stringtype < 0) return 0; /* MTF_NO_STRINGS */
	if (!mtfscan_ready(s, q.pos + q.size)) return 0;

	if (!(s->stringtype & 1)) {
		/* internally in UCS-2LE */

		/* find utf-8 length */
		for (i = n = 0; i < q.size; i += 2) {
			uc = (unsigned char *)&s->buffer[q.pos + i];
			if (uc[1] & 0xF8) n += 3;
			else if (uc[1] | (uc[0] & 0x80)) n += 2;
			else n++;
		}

		/* convert to UTF8 */
		p = out = (char *)malloc(n+1); /* for a terminating null */
		if (!p) return 0;
		for (i = n = 0; i < q.size; i += 2) {
			uc = (unsigned char *)&s->buffer[q.pos + i];
			if (!uc[0] && !uc[1]) {
				p[0] = sz & 0x7F;
				p++;
			} else if (uc[1] & 0xF8) {
				p[0] = 0xE0 | (uc[1] >> 4);
				p[1] = 0x80 | ((uc[1] & 15) << 2) | (uc[0] >> 6);
				p[2] = 0x80 | (uc[0] & 0x3F);
				p += 3;
			} else if (uc[1] | (uc[0] & 0x80)) {
				p[0] = 0xC0 | (uc[1] << 2);
				p[1] = 0x80 | (uc[0] & 0x3F);
				p += 2;
			} else {
				p[0] = uc[0];
				p++;
			}
		}
		p[0] = 0; /* nul */
	} else {
		/* ascii (actually MSDOS CP 646) */
		out = (char *)malloc(q.size+1);
		if (!out) return 0;
		memcpy(out, &s->buffer[q.pos], q.size);
		if (sz) {
			for (i = 0; i < q.size; i++)
				if (!out[i]) out[i] = sz;
		}
		out[q.size] = 0;
	}
	return out;
}

