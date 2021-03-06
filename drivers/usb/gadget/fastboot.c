/*
 * Copyright (c) 2009, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the 
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED 
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
//#define DEBUG
#include <config.h>
#include <common.h>
#include <asm/errno.h>
#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#include "gadget_chips.h"
#include <linux/ctype.h>
#include <malloc.h>
#include <command.h>
#include <nand.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/nand.h>
#include <jffs2/jffs2.h>
#include <asm/types.h>
#include <android_boot.h>
#include <android_bootimg.h>
#include <boot_mode.h>


#ifdef CONFIG_EXT4_SPARSE_DOWNLOAD
#include "../disk/part_uefi.h"
#include "../drivers/mmc/card_sdio.h"
#include "asm/arch/sci_types.h"
#include <ext_common.h>
#include <ext4fs.h>

#define MAGIC_DATA	0xAA55A5A5
#define SPL_CHECKSUM_LEN	0x6000
#define CHECKSUM_START_OFFSET	0x28
#define MAGIC_DATA_SAVE_OFFSET	(0x20/4)
#define CHECKSUM_SAVE_OFFSET	(0x24/4)
PARTITION_CFG uefi_part_info[MAX_PARTITION_INFO];
#define EFI_SECTOR_SIZE 		(512)
#define ERASE_SECTOR_SIZE		((64 * 1024) / EFI_SECTOR_SIZE)
#define EMMC_BUF_SIZE			(((216 * 1024 * 1024) / EFI_SECTOR_SIZE) * EFI_SECTOR_SIZE)
#if defined (CONFIG_SC8825) || defined (CONFIG_TIGER)
unsigned char *g_eMMCBuf = (unsigned char*)0x82000000;
#else
unsigned char *g_eMMCBuf = (unsigned char*)0x2000000;
#endif

#if defined CONFIG_SC8825
#define BOOTLOADER_HEADER_OFFSET 0x20
typedef struct{
	uint32 version;
	uint32 magicData;
	uint32 checkSum;
	uint32 hashLen;
}EMMC_BootHeader;
#endif

typedef struct
{
	unsigned int partition_index;
	unsigned int partition_type;
	char *partition_str;
} eMMC_Parttion;

eMMC_Parttion const _sprd_emmc_partition[]={
	{PARTITION_VM, PARTITION_USER, "vmjaluna"},
	{PARTITION_MODEM, PARTITION_USER, "modem"},
	{PARTITION_DSP, PARTITION_USER, "dsp"},
	{PARTITION_FIX_NV1, PARTITION_USER, "fixnv"},
	{PARTITION_KERNEL, PARTITION_USER, "boot"},
	{PARTITION_RECOVERY, PARTITION_USER, "recovery"},
	{PARTITION_SYSTEM, PARTITION_USER, "system"},
	{PARTITION_LOGO, PARTITION_USER, "boot_logo"},
	{PARTITION_USER_DAT, PARTITION_USER, "userdata"},
	{PARTITION_CACHE, PARTITION_USER, "cache"},
	{0, PARTITION_BOOT1, "params"},
	{0, PARTITION_BOOT2, "2ndbl"},
	{0,0,0}
};

#endif

typedef struct {
    unsigned char colParity;
    unsigned lineParity;
    unsigned lineParityPrime;
} yaffs_ECCOther;
typedef struct {
    unsigned sequenceNumber;
    unsigned objectId;
    unsigned chunkId;
    unsigned byteCount;
} yaffs_PackedTags2TagsPart;

typedef struct {
    yaffs_PackedTags2TagsPart t;
    yaffs_ECCOther ecc;
} yaffs_PackedTags2;

//#define FASTBOOT_DEBUG
#ifdef FASTBOOT_DEBUG
#define fb_printf(fmt, args...) printf(fmt, ##args)
#else
#define fb_printf(fmt, args...) do {} while(0)
#endif

#ifdef FLASH_PAGE_SIZE
#undef FLASH_PAGE_SIZE
#endif
#define FLASH_PAGE_SIZE 2048

#define ROUND_TO_PAGE(x,y) (((x) + (y)) & (~(y)))

int nand_do_write_ops(struct mtd_info *mtd, loff_t to,struct mtd_oob_ops *ops);
int nand_do_write_oob(struct mtd_info *mtd, loff_t to,
                             struct mtd_oob_ops *ops);

#define GFP_ATOMIC ((gfp_t) 0)
static int current_write_position;

int get_end_write_pos(void)
{
	return current_write_position;
}
void set_current_write_pos(int pos)
{
	current_write_position = pos;
}
void move2goodblk(void)
{
	while(1) fb_printf("suspend in move2goodblk\n");
}
/* todo: give lk strtoul and nuke this */
static unsigned hex2unsigned(const char *x)
{
    unsigned n = 0;

    while(*x) {
        switch(*x) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            n = (n << 4) | (*x - '0');
            break;
        case 'a': case 'b': case 'c':
        case 'd': case 'e': case 'f':
            n = (n << 4) | (*x - 'a' + 10);
            break;
        case 'A': case 'B': case 'C':
        case 'D': case 'E': case 'F':
            n = (n << 4) | (*x - 'A' + 10);
            break;
        default:
            return n;
        }
        x++;
    }

    return n;
}

struct fastboot_cmd {
	struct fastboot_cmd *next;
	const char *prefix;
	unsigned prefix_len;
	void (*handle)(const char *arg, void *data, unsigned sz);
};

struct fastboot_var {
	struct fastboot_var *next;
	const char *name;
	const char *value;
};
	
static struct fastboot_cmd *cmdlist;

void fastboot_register(const char *prefix,
		       void (*handle)(const char *arg, void *data, unsigned sz))
{
	struct fastboot_cmd *cmd;
	cmd = malloc(sizeof(*cmd));
	if (cmd) {
		cmd->prefix = prefix;
		cmd->prefix_len = strlen(prefix);
		cmd->handle = handle;
		cmd->next = cmdlist;
		cmdlist = cmd;
	}
}

static struct fastboot_var *varlist;

void fastboot_publish(const char *name, const char *value)
{
	struct fastboot_var *var;
	var = malloc(sizeof(*var));
	if (var) {
		var->name = name;
		var->value = value;
		var->next = varlist;
		varlist = var;
	}
}


//static event_t usb_online;
//static event_t txn_done;
static volatile int txn_done;
static unsigned char buffer[4096];
static struct usb_ep *in, *out;
//static struct udc_request *req;
static struct usb_request *tx_req, *rx_req;
int txn_status;

static void *download_base;
static unsigned download_max;
static unsigned download_size;

#define STATE_OFFLINE	0
#define STATE_COMMAND	1
#define STATE_COMPLETE	2
#define STATE_ERROR	3

static unsigned fastboot_state = STATE_OFFLINE;

//static void req_complete(struct udc_request *req, unsigned actual, int status)
static void req_complete(struct usb_ep *ep, struct usb_request *req)
{
	if (req->status || req->actual != req->length)
		debug("req complete --> %d, %d/%d\n",
				req->status, req->actual, req->length);
	
	txn_status = req->status;
	txn_done = 1;
	/*
	req->length = actual;
	event_signal(&txn_done, 0);
	*/
}

static int usb_read(void *_buf, unsigned len)
{
	int r;
	unsigned xfer;
	unsigned char *buf = _buf;
	int count = 0;
	struct usb_request * req = rx_req;
	
	if (fastboot_state == STATE_ERROR)
		goto oops;

	while (len > 0) {
		xfer = (len > 4096) ? 4096 : len;
		req->buf = buf;
		req->length = xfer;
		req->complete = req_complete;
		//r = udc_request_queue(out, req);
		r = usb_ep_queue(out, req, GFP_ATOMIC);
		if (r < 0) {
			printf("usb_read() queue failed\n");
			goto oops;
		}
		//event_wait(&txn_done);
		txn_done = 0;
		while(!txn_done)
			usb_gadget_handle_interrupts();

		if (txn_status < 0) {
			printf("usb_read() transaction failed\n");
			goto oops;
		}

		count += req->actual;
		buf += req->actual;
		len -= req->actual;

		/* short transfer? */
		if (req->actual != xfer) break;
	}

	return count;

oops:
	fastboot_state = STATE_ERROR;
	return -1;
}

static int usb_write(void *buf, unsigned len)
{
	int r;
	struct usb_request * req = tx_req;
	
	if (fastboot_state == STATE_ERROR)
		goto oops;

	req->buf = buf;
	req->length = len;
	req->complete = req_complete;
	txn_done = 0;
	//r = udc_request_queue(in, req);
	r = usb_ep_queue(in, req, GFP_ATOMIC);
	if (r < 0) {
		printf("usb_write() queue failed\n");
		goto oops;
	}
	//event_wait(&txn_done);
	while(!txn_done)
		usb_gadget_handle_interrupts();
	if (txn_status < 0) {
		printf("usb_write() transaction failed\n");
		goto oops;
	}
	return req->actual;

oops:
	fastboot_state = STATE_ERROR;
	return -1;
}

void fastboot_ack(const char *code, const char *reason)
{
	char response[64] = {0};

	if (fastboot_state != STATE_COMMAND)
		return;

	if (reason == 0)
		reason = "";

	//snprintf(response, 64, "%s%s", code, reason);
	if(strlen(code) + strlen(reason) >= 64) {
		printf("%s too long string\r\n", __func__);
	}
	sprintf(response, "%s%s", code, reason);
	fastboot_state = STATE_COMPLETE;

	usb_write(response, strlen(response));

}

void fastboot_fail(const char *reason)
{
	fastboot_ack("FAIL", reason);
}

void fastboot_okay(const char *info)
{
	fastboot_ack("OKAY", info);
}

static void cmd_getvar(const char *arg, void *data, unsigned sz)
{
	struct fastboot_var *var;

	for (var = varlist; var; var = var->next) {
		if (!strcmp(var->name, arg)) {
			fastboot_okay(var->value);
			return;
		}
	}
	fastboot_okay("");
}

static void dump_log(char * buf, int len)
{
	int i = 0;

	fb_printf("**dump log_buf ...addr:0x%08x, len:%d\r\n", buf, len);

	for (i = 0; i < len; i++)	{
		fb_printf("%02x ", *((unsigned char*)buf+i) );
		if(i%0x20 == 0x1f)
			fb_printf("\n");
	}
}
static void cmd_download(const char *arg, void *data, unsigned sz)
{
	char response[64];
	unsigned len = hex2unsigned(arg);
	int r;

	fb_printf("%s\n", __func__);
	
	fb_printf("arg'%s' data %p, %d\n",arg, data,sz);
	download_size = 0;
	if (len > download_max) {
		fastboot_fail("data too large");
		return;
	}

	sprintf(response,"DATA%08x", len);
	if (usb_write(response, strlen(response)) < 0)
		return;

	r = usb_read(download_base, len);
	if ((r < 0) || (r != len)) {
		fastboot_state = STATE_ERROR;
		return;
	}
	download_size = len;
	fastboot_okay("");
	//dump_log(download_base, len);
}

#ifdef CONFIG_EXT4_SPARSE_DOWNLOAD
unsigned short fastboot_eMMCCheckSum(const unsigned int *src, int len)
{
	unsigned int   sum = 0;
	unsigned short *src_short_ptr = PNULL;

	while (len > 3){
		sum += *src++;
		len -= 4;
	}
	src_short_ptr = (unsigned short *) src;
	if (0 != (len&0x2)){
		sum += * (src_short_ptr);
		src_short_ptr++;
	}
	if (0 != (len&0x1)){
		sum += * ( (unsigned char *) (src_short_ptr));
	}
	sum  = (sum >> 16) + (sum & 0x0FFFF);
	sum += (sum >> 16);

	return (unsigned short) (~sum);
}

void _add_4s(unsigned char *data, unsigned char add_word, int base_size){
	data[base_size + 0] = data[base_size + 1] = add_word;
	data[base_size + 2] = data[base_size + 3] = add_word;
}

int fastboot_flashNVParttion(EFI_PARTITION_INDEX part, void *data, size_t sz)
{
	unsigned long  fix_nv_checksum;
	unsigned long len, nblocknum;
	block_dev_desc_t *pdev;
	disk_partition_t info;

	//Set write para.
	len = FIXNV_SIZE + 4;
	_add_4s(data, 0x5a, FIXNV_SIZE);

	//Write to eMMC
	pdev = get_dev("mmc", 1);
	if (pdev == NULL) {
		fastboot_fail("Block device not supported!");
		return 0;
	}
	if (get_partition_info(pdev, part, &info)){
		fastboot_fail("eMMC get partition ERROR!");
		return 0;
	}
	if (sz % 512)
		nblocknum = sz / 512 + 1;
	else
		nblocknum = sz / 512;
	if (!Emmc_Write(PARTITION_USER, info.start,  nblocknum, data))
		return 0;

	return 1; /* success */
}

void fastboot_splFillCheckData(unsigned int * splBuf,  int len)
{
#if   defined(CONFIG_SC8810)
	*(splBuf + MAGIC_DATA_SAVE_OFFSET) = MAGIC_DATA;
	*(splBuf + CHECKSUM_SAVE_OFFSET) = (unsigned int)fastboot_eMMCCheckSum((unsigned int *)&splBuf[CHECKSUM_START_OFFSET/4], SPL_CHECKSUM_LEN - CHECKSUM_START_OFFSET);

#elif defined(CONFIG_SC8825) || defined(CONFIG_SC7710G2)
	EMMC_BootHeader *header;
	header = (EMMC_BootHeader *)((unsigned char*)splBuf+BOOTLOADER_HEADER_OFFSET);
	header->version  = 0;
	header->magicData= MAGIC_DATA;
	header->checkSum = (unsigned int)fastboot_eMMCCheckSum((unsigned char*)splBuf+BOOTLOADER_HEADER_OFFSET+sizeof(*header), SPL_CHECKSUM_LEN-(BOOTLOADER_HEADER_OFFSET+sizeof(*header)));
	header->hashLen  = 0;
#endif
}


void cmd_flash(const char *arg, void *data, unsigned sz)
{
	size_t size = 0;
	u8 pnum = 0;
	unsigned int nblocknum;
	int pos;

	data = download_base;
	size = sz;
	//Seek partition form _sprd_emmc_partition table
	for (pos = 0; pos < (sizeof(_sprd_emmc_partition) / sizeof(eMMC_Parttion)); pos++){
		if (!strcmp(_sprd_emmc_partition[pos].partition_str, arg))
			break;
		pnum++;
	}
	printf("Flash emmc partition:%s check:%s-%d\n", _sprd_emmc_partition[pos-1].partition_str, arg, pnum);
	if (pnum >= sizeof(_sprd_emmc_partition) / sizeof(eMMC_Parttion)){
		fastboot_fail("unknown partition name");
		return;
	}
	//Check boot&recovery img's magic
	if (!strcmp(_sprd_emmc_partition[pnum].partition_str, "boot")
		 ||!strcmp(_sprd_emmc_partition[pnum].partition_str, "recovery")) {
		if (memcmp((void *)data, BOOT_MAGIC, BOOT_MAGIC_SIZE)) {
			fastboot_fail("image is not a boot image");
			return;
		}
	}

	if ((_sprd_emmc_partition[pnum].partition_index== PARTITION_SYSTEM) \
		|| (_sprd_emmc_partition[pnum].partition_index == PARTITION_USER_DAT) \
		|| (_sprd_emmc_partition[pnum].partition_index == PARTITION_CACHE)){
		//Flash system&userdata - RAW ext4 img with sprase
		/* richardfeng add size to the function */
		if (write_simg2emmc("mmc", 1, _sprd_emmc_partition[pnum].partition_index, data, size) != 0){
			fastboot_fail("eMMC WRITE_ERROR!");
			return;
		}
	}else if (!strcmp(_sprd_emmc_partition[pnum].partition_str, "params")
			||!strcmp(_sprd_emmc_partition[pnum].partition_str, "2ndbl")){
		//Flash u-boot&spl in BOOT area
		if(size%512)
			nblocknum = size/512 + 1;
		else
			nblocknum = size/512;
		if(!strcmp(_sprd_emmc_partition[pnum].partition_str, "params")){
			fastboot_splFillCheckData(data, size);
			nblocknum = SPL_CHECKSUM_LEN/512;
		}
		if(!Emmc_Write(_sprd_emmc_partition[pnum].partition_type, 0,  nblocknum, data)){
			fastboot_fail("eMMC WRITE_ERROR!");
			return;
		}
	}else if (!strcmp(_sprd_emmc_partition[pnum].partition_str, "fixnv")) {
		if (!fastboot_flashNVParttion(_sprd_emmc_partition[pnum].partition_index, data, size)) {
			fastboot_fail("eMMC NV WRITE_ERROR!");
			return;
		}
	}else{
		//Flash other partitions - RAW img without sprase or filesystem
		block_dev_desc_t *pdev;
		disk_partition_t info;

		pdev = get_dev("mmc", 1);
		if (pdev == NULL) {
			fastboot_fail("Block device not supported!");
			return;
		}
		if (get_partition_info(pdev, _sprd_emmc_partition[pnum].partition_index, &info)){
			fastboot_fail("eMMC get partition ERROR!");
			return;
		}
		if(size%512)
			nblocknum = size/512 + 1;
		else
			nblocknum = size/512;
		if(!Emmc_Write(_sprd_emmc_partition[pnum].partition_type, info.start,  nblocknum, data)){
			fastboot_fail("eMMC WRITE_ERROR!");
			return;
		}
	}

	fastboot_okay("");
}

void cmd_erase(const char *arg, void *data, unsigned sz)
{
	u8 pnum = 0;
	unsigned int nblocknum;
	int pos;
	unsigned long len;
	unsigned long count;
	int i;
	size_t size;

	memset(g_eMMCBuf, 0xff, EMMC_BUF_SIZE);
	//Seek partition form _sprd_emmc_partition table
	for (pos = 0; pos < (sizeof(_sprd_emmc_partition) / sizeof(eMMC_Parttion)); pos++){
		if (!strcmp(_sprd_emmc_partition[pos].partition_str, arg))
			break;
		pnum++;
	}
	printf("Flash emmc partition:%s check:%s-%d\n", _sprd_emmc_partition[pos-1].partition_str, arg, pnum);
	if (pnum >= sizeof(_sprd_emmc_partition) / sizeof(eMMC_Parttion)){
		fastboot_fail("unknown partition name");
		return;
	}

	block_dev_desc_t *pdev;
	disk_partition_t info;

	pdev = get_dev("mmc", 1);
	if (pdev == NULL) {
		fastboot_fail("Block device not supported!");
		return;
	}
	if (get_partition_info(pdev, _sprd_emmc_partition[pnum].partition_index, &info)){
		fastboot_fail("eMMC get partition ERROR!");
		return;
	}

	size = info.size;
	
	if (size < EMMC_BUF_SIZE) {
		if(size%512)
			nblocknum = size/512 + 1;
		else
			nblocknum = size/512;
		if(!Emmc_Write(_sprd_emmc_partition[pnum].partition_type, info.start,  nblocknum, (unsigned char *)g_eMMCBuf)){
			fastboot_fail("eMMC WRITE_ERROR!");
			return;
		}
	}
	else {
		count = size / (EMMC_BUF_SIZE / EFI_SECTOR_SIZE);
		for (i = 0; i < count; i++) {
			if (!Emmc_Write(_sprd_emmc_partition[pnum].partition_type, info.start + i * (EMMC_BUF_SIZE / EFI_SECTOR_SIZE),
				EMMC_BUF_SIZE / EFI_SECTOR_SIZE, (unsigned char *)g_eMMCBuf))
				fastboot_fail("eMMC ERASE_ERROR!");
				return 0;
		}

		count = len % (EMMC_BUF_SIZE / EFI_SECTOR_SIZE);
		if (count) {
			if (!Emmc_Write(_sprd_emmc_partition[pnum].partition_type, info.start + i * (EMMC_BUF_SIZE / EFI_SECTOR_SIZE),
				count, (unsigned char *)g_eMMCBuf))
				return 0;
			}
	}
	fastboot_okay("");
}

#else
char tempBuf[9*1024]={0};
void cmd_flash(const char *arg, void *data, unsigned sz)
{
	struct mtd_info *nand;
    struct mtd_device *dev;
    struct part_info *part;
	size_t size = 0;
    u8 pnum;
	unsigned extra = 0;
    int ret;

	data = download_base; //previous downloaded date to download_base

	fb_printf("%s, arg:%x date: 0x%x, sz 0x%x\n", __func__, arg, data, sz);
    ret = mtdparts_init();
    if(ret != 0){
        fastboot_fail("mtdparts init error");
        return;
    }

    ret = find_dev_and_part(arg, &dev, &pnum, &part);
    if(ret){
		fastboot_fail("unknown partition name");
        return;
    }else if(dev->id->type != MTD_DEV_TYPE_NAND){
        fastboot_fail("mtd dev type error");
        return;
    }

	nand = &nand_info[dev->id->num];

    nand_erase_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.offset = (loff_t)part->offset;
    opts.length = (loff_t)part->size;
    opts.jffs2 = 0;
	opts.quiet = 1;

    fb_printf("opts off  0x%08x\n", (uint32_t)opts.offset);
    fb_printf("opts size 0x%08x\n", (uint32_t)opts.length);
	fb_printf("nand write size 0x%08x\n", nand->writesize);
    ret = nand_erase_opts(nand, &opts);

    if(ret){
      fastboot_fail("nand erase error");
      return;
    }
	
	if (!strcmp(part->name, "boot") || !strcmp(part->name, "recovery")) {
		if (memcmp((void *)data, BOOT_MAGIC, BOOT_MAGIC_SIZE)) {
			fastboot_fail("image is not a boot image");
			return;
		}
	}

	if (!strcmp(part->name, "system") || !strcmp(part->name, "userdata"))
		extra = 64;
	else
		sz = ROUND_TO_PAGE(sz, nand->writesize -1);

	size = sz;
	fb_printf("writing 0x%x bytes to '%s' offset: 0x%08x nand_curr_device=%d\n", size, part->name, part->offset,nand_curr_device);
    if(!extra){ // boot or recovery partition write
        ret = nand_write_skip_bad(nand, (loff_t)part->offset, &size, data);
    }else{
#if defined(CONFIG_SC7710G2) || defined(CONFIG_SP6821A)
        int part_off = part->offset;
        int copy_size = 12 * 1024;
        set_current_write_pos(part_off);
        init_yaffs_convert_variables(nand->writesize,nand->oobsize,1);

        set_convert_buffer(data,size);
        if (convert_buffer_is_full())
            yaffs2_convertAndWrite(0,nand->writesize,nand->oobsize,tempBuf);
        yaffs2_convertAndWrite(1,nand->writesize,nand->oobsize,tempBuf);
#else
        struct nand_chip *chip = nand->priv;
        chip->ops.mode = MTD_OOB_AUTO;
        chip->ops.len = nand->writesize;
        chip->ops.ooblen = sizeof(yaffs_PackedTags2);
        chip->ops.ooboffs = 0;
        loff_t part_off = (loff_t)part->offset;

        while(size){
            chip->ops.datbuf = (uint8_t *)data;
            chip->ops.oobbuf = (uint8_t *)(data + nand->writesize);
            if(!nand_block_isbad(nand, part_off)){
                ret = nand_do_write_ops(nand, part_off, &(chip->ops));
                if(ret){
                    //fastboot_fail("flash write failure");
                    break;
                    nand->block_markbad(nand, part_off);
                    part_off += nand->writesize;
                    continue;
                }
                data += (nand->writesize + nand->oobsize);
                size -= (nand->writesize + nand->oobsize);
            }
			part_off += nand->writesize;
        }
#endif
    }

	if(!ret)
		fastboot_okay("");
	else
		fastboot_fail("flash error");
}

void cmd_erase(const char *arg, void *data, unsigned sz)
{
	struct mtd_info *nand;
    struct mtd_device *dev;
    struct part_info *part;
    u8 pnum;
	unsigned extra = 0;
    int ret;

	fb_printf("%s\n", __func__);

    ret = mtdparts_init();
    if(ret != 0){
        fastboot_fail("mtdparts init error");
        return;
    }

    ret = find_dev_and_part(arg, &dev, &pnum, &part);
    if(ret){
		fastboot_fail("unknown partition name");
        return;
    }else if(dev->id->type != MTD_DEV_TYPE_NAND){
        fastboot_fail("mtd dev type error");
        return;
    }
	
    nand = &nand_info[dev->id->num];
    nand_erase_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.offset = (loff_t)part->offset;
    opts.length = (loff_t)part->size;
    opts.jffs2 = 0;
	opts.quiet = 1;

    fb_printf("opts off  0x%08x\n", (uint32_t)opts.offset);
    fb_printf("opts size 0x%08x\n", (uint32_t)opts.length);
	fb_printf("nand write size 0x%08x\n", nand->writesize);
    ret = nand_erase_opts(nand, &opts);
    if(ret)
      fastboot_fail("nand erase error");
    else
      fastboot_okay("");
}
#endif

extern void udc_power_off(void);

extern unsigned char raw_header[2048];

void cmd_boot(const char *arg, void *data, unsigned sz)
{
	boot_img_hdr *hdr = raw_header;
	unsigned kernel_actual;
	unsigned ramdisk_actual;
	unsigned kernel_addr;
	unsigned ramdisk_addr;
	char * cmdline;

	fb_printf("%s, arg: %s, data: %p, sz: 0x%x\n", __func__, arg, data, sz);
	memcpy(raw_header, data, 2048);
	if(memcmp(hdr->magic, BOOT_MAGIC, BOOT_MAGIC_SIZE)){
		fb_printf("boot image headr: %s\n", hdr->magic);
		fastboot_fail("bad boot image header");
		return;
	}
	kernel_actual= ROUND_TO_PAGE(hdr->kernel_size,(FLASH_PAGE_SIZE - 1));
	if(kernel_actual<=0){
		fastboot_fail("kernel image should not be zero");
		return;
	}
	ramdisk_actual= ROUND_TO_PAGE(hdr->ramdisk_size,(FLASH_PAGE_SIZE - 1));
	if(ramdisk_actual<0){
		fastboot_fail("ramdisk size error");
		return;
	}
	
	memcpy((void*)hdr->kernel_addr, (void *)data + FLASH_PAGE_SIZE, kernel_actual);
	memcpy((void*)hdr->ramdisk_addr, (void *)data + FLASH_PAGE_SIZE + kernel_actual, ramdisk_actual);
	
	fb_printf("kernel @0x%08x (0x%08x bytes)\n", hdr->kernel_addr, kernel_actual);
	fb_printf("ramdisk @0x%08x (0x%08x bytes)\n", hdr->ramdisk_addr, ramdisk_actual);
	//set boot environment
	if(hdr->cmdline[0]){
		cmdline = (char *)hdr->cmdline;
	}else{
		cmdline = getenv("bootargs");
	}
	fb_printf("cmdline %s\n", cmdline);

	fastboot_okay("");
	udc_power_off();
	creat_atags(hdr->tags_addr, cmdline, hdr->ramdisk_addr, hdr->ramdisk_size);
	boot_linux(hdr->kernel_addr,hdr->tags_addr);
}

extern int do_cboot(cmd_tbl_t *cmdtp, int flag, int argc, char *const argv[]);

void cmd_continue(const char *arg, void *data, unsigned sz)
{
	fastboot_okay("");
	udc_power_off();
	//do_cboot(NULL, 0, 1, NULL);
    normal_mode();
}

void cmd_reboot(const char *arg, void *data, unsigned sz)
{
	fastboot_okay("");
	//udc_power_off();
    reboot_devices(NORMAL_MODE);
}

void cmd_reboot_bootloader(const char *arg, void *data, unsigned sz)
{
	fastboot_okay("");
	udc_power_off();
    reboot_devices(FASTBOOT_MODE);
}

void cmd_powerdown(const char *arg, void *data, unsigned sz)
{
	fastboot_okay("");
    power_down_devices(0);

}

static void fastboot_command_loop(void)
{
	struct fastboot_cmd *cmd;
	int r;
	fb_printf("fastboot: processing commands\n");

again:
	while (fastboot_state != STATE_ERROR) {
		memset(buffer, 0 , 64);
		r = usb_read(buffer, 64);
		if (r < 0) break;
		buffer[r] = 0;
		fb_printf("fastboot: %s, r:%d\n", buffer, r);

		for (cmd = cmdlist; cmd; cmd = cmd->next) {
			fb_printf("cmd list :%s \n", cmd->prefix);
			if (memcmp(buffer, cmd->prefix, cmd->prefix_len))
				continue;
			fastboot_state = STATE_COMMAND;
			cmd->handle((const char*) buffer + cmd->prefix_len,
				    (void*) download_base, download_size);
			if (fastboot_state == STATE_COMMAND)
				fastboot_fail("unknown reason");
			goto again;
		}

		fastboot_fail("unknown command");
			
	}
	fastboot_state = STATE_OFFLINE;
	fb_printf("fastboot: oops!\n");
}

static int fastboot_handler(void *arg)
{
	for (;;) {
		fastboot_command_loop();
	}
	return 0;
}

/*
static void fastboot_notify(struct udc_gadget *gadget, unsigned event)
{
	if (event == UDC_EVENT_ONLINE) {
		event_signal(&usb_online, 0);
	}
}

static struct udc_endpoint *fastboot_endpoints[2];

static struct udc_gadget fastboot_gadget = {
	.notify		= fastboot_notify,
	.ifc_class	= 0xff,
	.ifc_subclass	= 0x42,
	.ifc_protocol	= 0x03,
	.ifc_endpoints	= 2,
	.ifc_string	= "fastboot",
	.ept		= fastboot_endpoints,
};
*/
#if defined(CONFIG_CMD_FASTBOOT)
int do_fastboot (cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	fb_printf("%s is alive\n", __func__);
	while(1){
		usb_gadget_handle_interrupts();
	}
	
	return 0;
}

U_BOOT_CMD(
	fastboot,	1,	1,	do_fastboot,
	"android fastboot protocol",
);
#endif

int fastboot_init(void *base, unsigned size, struct usb_ep * ep_in, struct usb_ep *ep_out)
{
	printf("fastboot_init()\n");

	download_base = base;
	download_max = size;
	if(!ep_in) {
		printf("ep in is not alloc\r\n");
	}
	in = ep_in;
	
	if(!ep_out) {
		printf("ep out is not alloc\r\n");
	}
	out = ep_out;

	tx_req = usb_ep_alloc_request(in, 0);
	rx_req =  usb_ep_alloc_request(out, 0);
/*
	in = udc_endpoint_alloc(UDC_TYPE_BULK_IN, 512);
	if (!in)
		goto fail_alloc_in;
	out = udc_endpoint_alloc(UDC_TYPE_BULK_OUT, 512);
	if (!out)
		goto fail_alloc_out;

	fastboot_endpoints[0] = in;
	fastboot_endpoints[1] = out;

	req = udc_request_alloc();
	if (!req)
		goto fail_alloc_req;

	if (udc_register_gadget(&fastboot_gadget))
		goto fail_udc_register;
*/
/*
	static char cmd1[] = "getvar:";
	fastboot_register(cmd1, cmd_getvar);
*/
	fastboot_register("getvar:", cmd_getvar);
	fastboot_register("download:", cmd_download);
	//fastboot_register("flash:", cmd_flash);
	fastboot_publish("version", "1.0");

	fastboot_register("flash:", cmd_flash);
    fastboot_register("erase:", cmd_erase);
	fastboot_register("boot", cmd_boot);
	fastboot_register("reboot", cmd_reboot);
	fastboot_register("powerdown", cmd_powerdown);
	fastboot_register("continue", cmd_continue);
	fastboot_register("reboot-bootloader", cmd_reboot_bootloader);
	
    //fastboot_register(
	fastboot_handler(0);

	return 0;
/*
fail_udc_register:
	udc_request_free(req);
fail_alloc_req:
	udc_endpoint_free(out);	
fail_alloc_out:
	udc_endpoint_free(in);
fail_alloc_in:
	return -1;
*/
}

