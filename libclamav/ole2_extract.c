/*
 *  Extract component parts of OLE2 files (e.g. MS Office Documents)
 *
 *  Copyright (C) 2004 trog@uncon.org
 *
 *  This code is based on the OpenOffice and libgsf sources.
 *                  
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <stdlib.h>
#include <clamav.h>

#include "cltypes.h"

#define FALSE (0)
#define TRUE (1)

#define MIN(a, b)  (((a) < (b)) ? (a) : (b))

#ifdef WORDS_LITTLEENDIAN
#define ole2_endian_convert_16(v)	(v)
#warning Little Endian
#else
static uint16_t ole2_endian_convert_16(uint16_t v)
{
	return ((v >> 8) + (v << 8));
}
#warning Big Endian
#endif

#ifdef WORDS_LITTLEENDIAN
#define ole2_endian_convert_32(v)    (v)
#else
static uint32_t ole2_endian_convert_32(uint32_t v)
{
        return ((v >> 24) | ((v & 0x00FF0000) >> 8) |
                ((v & 0x0000FF00) << 8) | (v << 24));
}
#endif

typedef struct ole2_header_tag
{
	unsigned char magic[8];			/* should be: 0xd0cf11e0a1b11ae1 */
	unsigned char clsid[16];
	uint16_t minor_version;
	uint16_t dll_version;
	int16_t byte_order;			/* -2=intel */

	uint16_t log2_big_block_size;		/* usually 9 (2^9 = 512) */
	uint32_t log2_small_block_size;		/* usually 6 (2^6 = 128) */

	int32_t reserved[2];
	int32_t bat_count;
	int32_t prop_start;

	uint32_t signature;
	uint32_t sbat_cutoff;			/* cutoff for files held in small blocks (4096) */

	int32_t sbat_start;
	int32_t sbat_block_count;
	int32_t xbat_start;
	int32_t xbat_count;
	int32_t bat_array[109];

	/* not part of the ole2 header, but stuff we need in order to decode */
	/* must take account of the size of variables below here when
	   reading the header */
	int sbat_root_start;
} ole2_header_t __attribute__ ((packed));

typedef struct property_tag
{
	unsigned char name[64];			/* in unicode */
	int16_t name_size;
	unsigned char type;			/* 1=dir 2=file 5=root */
	unsigned char color;			/* black or red */
	int32_t prev;
	int32_t next;
	int32_t child;

	unsigned char clsid[16];
	uint16_t user_flags;

	uint32_t create_lowdate;
	uint32_t create_highdate;
	uint32_t mod_lowdate;
	uint32_t mod_highdate;
	int32_t start_block;
	int32_t size;
	unsigned char reserved[4];
} property_t __attribute__ ((packed));

char magic_id[] = { 0xd0, 0xcf, 0x11, 0xe0, 0xa1, 0xb1, 0x1a, 0xe1};


/* Function: readn
	Try hard to read the requested number of bytes
*/
int readn(int fd, void *buff, unsigned int count)
{
	int retval;
	unsigned int todo;
	void *current;

	todo = count;
	current = buff;

	do {
		retval = read(fd, current, todo);
		if (retval == 0) {
			return (count - todo);
		}
		if (retval < 0) {
			return -1;
		}
		todo -= retval;
		current += retval;
	} while (todo > 0);

	return count;
}

/* Function: writen
	Try hard to write the specified number of bytes
*/
int writen(int fd, void *buff, unsigned int count)
{
	int retval;
	unsigned int todo;
	void *current;

	todo = count;
	current = buff;

	do {
		retval = write(fd, current, todo);
		if (retval < 0) {
			return -1;
		}
		todo -= retval;
		current += retval;
	} while (todo > 0);

	return count;
}

void print_property_name(char *name, int size)
{
	int i, count=0;

	if (*name == 0 || size == 0) {
		cli_dbgmsg("[no name]                           ");
		return;
	}
	/* size-2 to ignore trailing NULL */
	for (i=0 ; i<size-2; i+=2) {
		if (isprint(name[i])) {
			cli_dbgmsg("%c", name[i]);
			count++;
		} else {
			cli_dbgmsg("_%d_", name[i]);
			count += 3;
		}
	}
	for (i=0 ; i < (34-count) ; i++) {
		cli_dbgmsg(" ");
	}
}

char *get_property_name(char *name, int size)
{
	int i, j;
	char *newname;

	if (*name == 0 || size == 0) {
		return NULL;
	}

	newname = (char *) cli_malloc(size*2);
	if (!newname) {
		return NULL;
	}
	j=0;
	/* size-2 to ignore trailing NULL */
	for (i=0 ; i < size-2; i+=2) {
		if (isprint(name[i])) {
			newname[j++] = name[i];
		} else {
			if (name[i] < 10 && name[i] >= 0) {
				newname[j++] = '_';
				newname[j++] = name[i] + '0';
			}
			newname[j++] = '_';
		}
	}
	newname[j] = '\0';
	if (strlen(newname) == 0) {
		printf ("zero sized newname\n");
		free(newname);
		return NULL;
	}
	return newname;
}

void print_ole2_property(property_t *property)
{
	//print_property_name(property->name, property->name_size);
	switch (property->type) {
	case 2:
		cli_dbgmsg(" [file]");
		break;
	case 1:
		cli_dbgmsg(" [dir ]");
		break;
	case 5:
		cli_dbgmsg(" [root]");
		break;
	default:
		cli_dbgmsg(" [%d]", property->type);
	}
	switch (property->color) {
	case 0:
		cli_dbgmsg(" r");
		break;
	case 1:
		cli_dbgmsg(" b");
		break;
	default:
		cli_dbgmsg(" u");
	}
	cli_dbgmsg(" %d %x\n", property->size, property->user_flags);
}

void print_ole2_header(ole2_header_t *hdr)
{
	int i;
	
	if (!hdr) {
		return;
	}
	
	cli_dbgmsg("\nMagic:\t\t\t0x");
	for (i=0 ; i<8; i++) {
		cli_dbgmsg("%x", hdr->magic[i]);
	}
	cli_dbgmsg("\n");

	cli_dbgmsg("CLSID:\t\t\t{");
	for (i=0 ; i<16; i++) {
		cli_dbgmsg("%x ", hdr->clsid[i]);
	}
	cli_dbgmsg("}\n");

	cli_dbgmsg("Minor version:\t\t0x%x\n", hdr->minor_version);
	cli_dbgmsg("DLL version:\t\t0x%x\n", hdr->dll_version);
	cli_dbgmsg("Byte Order:\t\t%d\n", hdr->byte_order);
	cli_dbgmsg("Big Block Size:\t\t%i\n", hdr->log2_big_block_size);
	cli_dbgmsg("Small Block Size:\t%i\n", hdr->log2_small_block_size);
	cli_dbgmsg("BAT count:\t\t%d\n", hdr->bat_count);
	cli_dbgmsg("Prop start:\t\t%d\n", hdr->prop_start);
	cli_dbgmsg("SBAT cutoff:\t\t%d\n", hdr->sbat_cutoff);
	cli_dbgmsg("SBat start:\t\t%d\n", hdr->sbat_start);
	cli_dbgmsg("SBat block count:\t%d\n", hdr->sbat_block_count);
	cli_dbgmsg("XBat start:\t\t%d\n", hdr->xbat_start);
	cli_dbgmsg("XBat block count:\t%d\n\n", hdr->xbat_count);
	return;
}

int ole2_read_block(int fd, ole2_header_t *hdr, void *buff, int blockno)
{
	int offset;

	// other methods: (blockno+1) * 512 or (blockno * block_size) + 512;
	offset = (blockno << hdr->log2_big_block_size) + 512;	/* 512 is header size */
	if (lseek(fd, offset, SEEK_SET) != offset) {
		return FALSE;
	}
	if (readn(fd, buff, (1 << hdr->log2_big_block_size)) != (1 << hdr->log2_big_block_size)) {
		return FALSE;
	}
	return TRUE;
}

int ole2_get_next_bat_block(int fd, ole2_header_t *hdr, int current_block)
{
	int bat_array_index;
	uint32_t bat[128];

	bat_array_index = current_block / 128;
	if (bat_array_index > hdr->bat_count) {
		cli_dbgmsg("bat_array index error\n");
		return -10;
	}
	ole2_read_block(fd, hdr, &bat, ole2_endian_convert_32(hdr->bat_array[bat_array_index]));
	return ole2_endian_convert_32(bat[current_block-(bat_array_index * 128)]);
}

int ole2_get_next_sbat_block(int fd, ole2_header_t *hdr, int current_block)
{
	int iter, current_bat_block;
	uint32_t sbat[128];

	current_bat_block = hdr->sbat_start;
	iter = current_block / 128;
	while (iter > 0) {
		current_bat_block = ole2_get_next_bat_block(fd, hdr, current_bat_block);
		iter--;
	}
	ole2_read_block(fd, hdr, &sbat, current_bat_block);
	return ole2_endian_convert_32(sbat[current_block % 128]);
}

int ole2_get_next_xbat_block(int fd, ole2_header_t *hdr, int current_block)
{
	int xbat_index, xbat_block_index, bat_index, bat_blockno;
	uint32_t xbat[128], bat[128];

	xbat_index = current_block / 128;

	/* NB:	The last entry in each XBAT points to the next XBAT block.
		This reduces the number of entries in each block by 1.
	*/
	xbat_block_index = (xbat_index - 109) / 127;
	bat_blockno = (xbat_index - 109) % 127;

	bat_index = current_block % 128;

	ole2_read_block(fd, hdr, &xbat, hdr->xbat_start);

	/* Follow the chain of XBAT blocks */
	while (xbat_block_index > 0) {
		ole2_read_block(fd, hdr, &xbat, ole2_endian_convert_32(xbat[127]));
		xbat_block_index--;
	}

	ole2_read_block(fd, hdr, &bat, xbat[bat_blockno]);

	return ole2_endian_convert_32(bat[bat_index]);
}

int ole2_get_next_block_number(int fd, ole2_header_t *hdr, int current_block)
{
	if ((current_block / 128) > 108) {
		return ole2_get_next_xbat_block(fd, hdr, current_block);
	} else {
		return ole2_get_next_bat_block(fd, hdr, current_block);
	}
}

/* Retrieve the block containing the data for the given sbat index */
int ole2_get_sbat_data_block(int fd, ole2_header_t *hdr, void *buff, int sbat_index)
{
	int block_count;
	int current_block;

	if (hdr->sbat_root_start < 0) {
		cli_errmsg("No root start block\n");
		return FALSE;
	}

	block_count = sbat_index / 8;			// 8 small blocks per big block
	current_block = hdr->sbat_root_start;
	while (block_count > 0) {
		current_block = ole2_get_next_bat_block(fd, hdr, current_block);
		block_count--;
	}
	/* current_block now contains the block number of the sbat array
	   containing the entry for the required small block */

	return(ole2_read_block(fd, hdr, buff, current_block));

}

/* Read the property tree.
   It is read as just an array rather than a tree */
void ole2_read_property_tree(int fd, ole2_header_t *hdr, const char *dir,
				void (*handler)(int fd, ole2_header_t *hdr, property_t *prop, const char *dir))
{
	property_t prop_block[4];
	int index, current_block;
	
	current_block = hdr->prop_start;

	while(current_block >= 0) {
		ole2_read_block(fd, hdr, prop_block, current_block);
		for (index=0 ; index < 4 ; index++) {
			if (prop_block[index].type > 0) {
				prop_block[index].name_size = ole2_endian_convert_16(prop_block[index].name_size);
				prop_block[index].prev = ole2_endian_convert_32(prop_block[index].prev);
				prop_block[index].next = ole2_endian_convert_32(prop_block[index].next);
				prop_block[index].child = ole2_endian_convert_32(prop_block[index].child);
				prop_block[index].user_flags = ole2_endian_convert_16(prop_block[index].user_flags);
				prop_block[index].create_lowdate = ole2_endian_convert_32(prop_block[index].create_lowdate);
				prop_block[index].create_highdate = ole2_endian_convert_32(prop_block[index].create_highdate);
				prop_block[index].mod_lowdate = ole2_endian_convert_32(prop_block[index].mod_lowdate);
				prop_block[index].mod_highdate = ole2_endian_convert_32(prop_block[index].mod_highdate);
				prop_block[index].start_block = ole2_endian_convert_32(prop_block[index].start_block);
				prop_block[index].size = ole2_endian_convert_32(prop_block[index].size);
				if (prop_block[index].type == 5) {
					hdr->sbat_root_start = prop_block[index].start_block;
				}
				print_ole2_property(&prop_block[index]);
				handler(fd, hdr, &prop_block[index], dir);
			}
		}
		current_block = ole2_get_next_block_number(fd, hdr, current_block);
	}
	return;
}

/* Callback handlers
   These are called for each entry in the container (property tree) */

/* Null Handler - doesn't do anything */
void handler_null(int fd, ole2_header_t *hdr, property_t *prop, const char *dir)
{
	return;
}

/* Write file Handler - write the contents of the entry to a file */
void handler_writefile(int fd, ole2_header_t *hdr, property_t *prop, const char *dir)
{
	unsigned char buff[(1 << hdr->log2_big_block_size)];
	int current_block, ofd, len, offset;
	char *name, *newname;

	if (prop->type != 2) {
		// Not a file
		return;
	}

	if (! (name = get_property_name(prop->name, prop->name_size))) {
		/* File without a name - create a name for it */
		int i;
                                                                                                                            
		i = lseek(fd, 0, SEEK_CUR);
		name = malloc(11);
		if (!name) {
			return;
		}
		snprintf(name, 10, "%.10d", i + (int) prop);
	}

	newname = (char *) cli_malloc(strlen(name) + strlen(dir) + 2);
	sprintf(newname, "%s/%s", dir, name);
	free(name);

	ofd = open(newname, O_WRONLY|O_CREAT|O_TRUNC, S_IRWXU);
	if (ofd < 0) {
		return;
	}
	free(newname);
	current_block = prop->start_block;
	len = prop->size;

	while((current_block >= 0) && (len > 0)) {
		if (prop->size < hdr->sbat_cutoff) {
			// Small block file
			if (!ole2_get_sbat_data_block(fd, hdr, &buff, current_block)) {
				cli_dbgmsg("ole2_get_sbat_data_block failed\n");
				close(ofd);
				return;
			}
			// buff now contains the block with 8 small blocks in it
			offset = 64 * (current_block % 8);
			if (writen(ofd, &buff[offset], MIN(len,64)) != MIN(len,64)) {
				close(ofd);
				return;
			}

			len -= MIN(len,64);
			current_block = ole2_get_next_sbat_block(fd, hdr, current_block);
		} else {
			// Big block file
			if (!ole2_read_block(fd, hdr, &buff, current_block)) {
				close(ofd);
				return;
			}
			if (writen(ofd, &buff, MIN(len,(1 << hdr->log2_big_block_size))) !=
							MIN(len,(1 << hdr->log2_big_block_size))) {
				close(ofd);
				return;
			}

			current_block = ole2_get_next_block_number(fd, hdr, current_block);
			len -= MIN(len,(1 << hdr->log2_big_block_size));
		}
	}
	close(ofd);
	return;
}

int cli_ole2_extract(int fd, const char *dirname)
{
	ole2_header_t hdr;

	cli_dbgmsg("in cli_ole2_extract()\n");

	/* size of header - size of other values in struct */
	readn(fd, &hdr, sizeof(struct ole2_header_tag) - sizeof(int));

	hdr.minor_version = ole2_endian_convert_16(hdr.minor_version);
	hdr.dll_version = ole2_endian_convert_16(hdr.dll_version);
	hdr.byte_order = ole2_endian_convert_16(hdr.byte_order);
	hdr.log2_big_block_size = ole2_endian_convert_16(hdr.log2_big_block_size);
	hdr.log2_small_block_size = ole2_endian_convert_32(hdr.log2_small_block_size);
	hdr.bat_count = ole2_endian_convert_32(hdr.bat_count);
	hdr.prop_start = ole2_endian_convert_32(hdr.prop_start);
	hdr.sbat_cutoff = ole2_endian_convert_32(hdr.sbat_cutoff);
	hdr.sbat_start = ole2_endian_convert_32(hdr.sbat_start);
	hdr.sbat_block_count = ole2_endian_convert_32(hdr.sbat_block_count);
	hdr.xbat_start = ole2_endian_convert_32(hdr.xbat_start);
	hdr.xbat_count = ole2_endian_convert_32(hdr.xbat_count);

	hdr.sbat_root_start = -1;

	if (strncmp(hdr.magic, magic_id, 8) != 0) {
		cli_dbgmsg("OLE2 magic failed!\n");
		return CL_EOLE2;
	}

	if (hdr.log2_big_block_size != 9) {
		cli_dbgmsg("WARNING: untested big block size - please report\n\n");
	}
	if (hdr.log2_small_block_size != 6) {
		cli_dbgmsg("WARNING: untested small block size - please report\n\n");
	}
	if (hdr.sbat_cutoff != 4096) {
		cli_dbgmsg("WARNING: untested sbat cutoff - please report\n\n");
	}

	print_ole2_header(&hdr);

	ole2_read_property_tree(fd, &hdr, dirname, handler_writefile);

	return 0;
}
