/*
 *  fixpng.c
 *
 *  Copyright 2007 MHW. Read the GPL v2.0 for legal details.
 *  Copyright 2012 Bastien Nocera <hadess@hadess.net>
 *  http://www.gnu.org/licenses/gpl-2.0.txt
 *
 *
 *  This tool will convert iPhone PNGs from its weird, non-compatible format to
 *  A format any PNG-compatible application will read. It will not flip the R
 *  and B channels.
 *
 *  In summary, this tool takes an input png uncompresses the IDAT chunk, recompresses
 *  it in a PNG-compatible way and then writes everything except the, so far,
 *  useless CgBI chunk to the output.
 *
 *  It's a relatively quick hack, and it will break if the IDAT in either form
 *  (compressed or uncompressed) is larger than 1MB, and if there are more than 20
 *  chunks before the IDAT(s). In that case, poke at MAX_CHUNKS and BUFSIZE.
 *
 *  Usage therefore: fixpng <input.png> <output.png>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <errno.h>

#include <zlib.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#define MAX_CHUNKS 20
#define BUFSIZE 1048576 // 1MB buffer size

typedef struct png_chunk_t {
	guint32 length;
	unsigned char *name;
	unsigned char *data;
	guint32 crc;
} png_chunk;

unsigned char pngheader[8] = {137, 80, 78, 71, 13, 10, 26, 10};
unsigned char datachunk[4] = {0x49, 0x44, 0x41, 0x54}; // IDAT
unsigned char endchunk[4] = {0x49, 0x45, 0x4e, 0x44}; // IEND
unsigned char cgbichunk[4] = {0x43, 0x67, 0x42, 0x49}; // CgBI

void die(char *why);
int check_png_header(unsigned char *);
static GList *read_chunks (unsigned char* buf);
void process_chunks(GList *chunks);
void write_png(GList *chunks, char *filename);
unsigned long mycrc(unsigned char *, unsigned char *, int);

int main(int argc, char **argv){
	char *buf;
	GList *chunks, *l;
	GError *error = NULL;

	if (argc!=3) {
		printf("Usage: %s <input> <output>\n\n",argv[0]);
		exit(1);
	}

	if (g_file_get_contents (argv[1], &buf, NULL, &error) == FALSE) {
		g_warning ("Couldn't read file '%s': %s", argv[1], error->message);
		g_error_free (error);
		return 1;
	}

	if (!check_png_header(buf)){
		die("This is not a PNG file. I require a PNG file!\n");
	}

	chunks = read_chunks(buf);
	process_chunks(chunks);
	write_png(chunks, argv[2]);
}

int check_png_header(unsigned char *buf){
	return (!(int) memcmp(buf, pngheader, 8));
}

static GList *
read_chunks (unsigned char* buf)
{
	GList *chunks = NULL;
	int i = 0;

	buf += 8;
	do {
		png_chunk *chunk;

		chunk = g_new0 (png_chunk, 1);

		memcpy(&chunk->length, buf, 4);
		chunk->length = ntohl(chunk->length);
		chunk->data = (unsigned char *)malloc(chunk->length);
		chunk->name = (unsigned char *)malloc(4);

		buf += 4;
		memcpy(chunk->name, buf, 4);
		buf += 4;
		chunk->data = (unsigned char *)malloc(chunk->length);
		memcpy(chunk->data, buf, chunk->length);
		buf += chunk->length;
		memcpy(&chunk->crc, buf, 4);
		chunk->crc = ntohl(chunk->crc);
		buf += 4;

		printf("Found chunk: %c%c%c%c\n", chunk->name[0], chunk->name[1], chunk->name[2], chunk->name[3]);
		printf("Length: %d, CRC32: %08x\n", chunk->length, chunk->crc);

		chunks = g_list_prepend (chunks, chunk);

		if (!memcmp(chunk->name, endchunk, 4)){
			// End of img.
			break;
		}
	} while (i++ < MAX_CHUNKS);

	return g_list_reverse (chunks);
}

void process_chunks(GList *chunks){
	GList *l;
	int i;
	unsigned char* outbuf;

	// Poke at any IDAT chunks and de/recompress them
	for (l = chunks; l != NULL; l = l->next) {
		png_chunk *chunk;
		int ret;
		z_stream infstrm, defstrm;
		unsigned char *inflatedbuf;
		unsigned char *deflatedbuf;

		chunk = l->data;

		/* End chunk */
		if (memcmp(chunk->name, endchunk, 4) == 0)
			break;

		/* Not IDAT */
		if (memcmp(chunk->name, datachunk, 4) != 0)
			continue;

		inflatedbuf = (unsigned char *)malloc(BUFSIZE);
		printf("processing IDAT chunk %d\n", i);
		infstrm.zalloc = Z_NULL;
		infstrm.zfree = Z_NULL;
		infstrm.opaque = Z_NULL;
		infstrm.avail_in = chunk->length;
		infstrm.next_in = chunk->data;
		infstrm.next_out = inflatedbuf;
		infstrm.avail_out = BUFSIZE;

		// Inflate using raw inflation
		if (inflateInit2(&infstrm,-8) != Z_OK){
			die("ZLib error");
		}

		ret = inflate(&infstrm, Z_NO_FLUSH);
		switch (ret) {
		case Z_NEED_DICT:
			ret = Z_DATA_ERROR;     /* and fall through */
		case Z_DATA_ERROR:
		case Z_MEM_ERROR:
			printf("ZLib error! %d\n", ret);
			inflateEnd(&infstrm);
		}

		inflateEnd(&infstrm);

		// Now deflate again, the regular, PNG-compatible, way
		deflatedbuf = (unsigned char *)malloc(BUFSIZE);

		defstrm.zalloc = Z_NULL;
		defstrm.zfree = Z_NULL;
		defstrm.opaque = Z_NULL;
		defstrm.avail_in = infstrm.total_out;
		defstrm.next_in = inflatedbuf;
		defstrm.next_out = deflatedbuf;
		defstrm.avail_out = BUFSIZE;

		deflateInit(&defstrm, Z_DEFAULT_COMPRESSION);
		deflate(&defstrm, Z_FINISH);

		chunk->data = deflatedbuf;
		chunk->length = defstrm.total_out;
		chunk->crc = mycrc(chunk->name, chunk->data, chunk->length);

		printf("Chunk: %c%c%c%c, new length: %d, new CRC: %08x\n",
		       chunk->name[0], chunk->name[1],
		       chunk->name[2], chunk->name[3],
		       chunk->length, chunk->crc);
	}
}

void write_png(GList *chunks, char *filename)
{
	int fd, i = 0;
	GList *l;

	fd = open(filename, O_CREAT|O_RDWR, S_IRUSR|S_IWUSR);
	write(fd, pngheader, 8);

	int did_idat = 0;

	for (l = chunks; l != NULL; l = l->next) {
		png_chunk *chunk;
		int tmp;

		chunk = l->data;

		tmp = htonl(chunk->length);
		chunk->crc = htonl(chunk->crc);

		if (memcmp(chunk->name, cgbichunk, 4)){ // Anything but a CgBI
			int ret;

			if (memcmp(chunk->name, "IDAT", 4) == 0) {
				if (did_idat == 1) {
					continue;
				}
				did_idat = 1;
			}

			ret = write(fd, &tmp, 4);
			ret = write(fd, chunk->name, 4);

			if (chunk->length > 0){
				printf("About to write data to fd length %d\n", chunk->length);
				ret = write(fd, chunk->data, chunk->length);
				if (!ret){
					printf("%c%c%c%c size %d\n", chunk->name[0], chunk->name[1], chunk->name[2], chunk->name[3], chunk->length);
					perror("write");
				}
			}

			ret = write(fd, &chunk->crc, 4);
		}

		if (!memcmp(chunk->name, endchunk, 4)){
			break;
		}
	}

	close(fd);
}

void die(char *why){
	printf(why);
	exit(1);
}

unsigned long mycrc(unsigned char *name, unsigned char *buf, int len)
{
	guint32 crc;

	crc = crc32(0, name, 4);
	return crc32(crc, buf, len);
}
