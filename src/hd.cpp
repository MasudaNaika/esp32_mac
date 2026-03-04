/*
 * SCSI HD emulation using ESP32-S3 flash partition.
 * Adapted from minimacplus esp32 hd.c
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "esp_partition.h"

extern "C" {
#include "tme/ncr.h"
#include "tme/hd.h"
}

typedef struct {
    const esp_partition_t* part;
    int size;
} HdPriv;

static const uint8_t inq_resp[95]={
    0, //HD
    0, //0x80 if removable
    0x49, //Obsolete SCSI standard 1 all the way
    0, //response version etc
    31, //extra data
    0,0, //reserved
    0, //features
    'A','P','P','L','E',' ',' ',' ', //vendor id
    '2','0','S','C',' ',' ',' ',' ', //prod id
    '1','.','0',' ',' ',' ',' ',' ', //prod rev lvl
};

static void writeSector(HdPriv *hd, unsigned int lba, uint8_t *data) {
    uint8_t *secdat=(uint8_t*)malloc(4096);
    assert(secdat);
    unsigned int lbaStart=lba&(~7);
    unsigned int lbaOff=lba&7;
    assert(esp_partition_read(hd->part, lbaStart*512, secdat, 4096)==ESP_OK);
    assert(esp_partition_erase_range(hd->part, lbaStart*512, 4096)==ESP_OK);
    for (int i=0; i<512; i++) secdat[lbaOff*512+i]=data[i];
    assert(esp_partition_write(hd->part, lbaStart*512, secdat, 4096)==ESP_OK);
    free(secdat);
}

static int hdScsiCmd(SCSITransferData *data, unsigned int cmd, unsigned int len, unsigned int lba, void *arg) {
    int ret=0;
    HdPriv *hd=(HdPriv*)arg;
    if (cmd==0x8 || cmd==0x28) { //read
        printf("HD: Read %d bytes from LBA %d.\n", len*512, lba);
        assert(esp_partition_read(hd->part, lba*512, data->data, len*512)==ESP_OK);
        ret=len*512;
    } else if (cmd==0x0A || cmd==0x2A) { //write
        uint8_t *dp=data->data;
        while(len) {
            writeSector(hd, lba, dp);
            lba++;
            dp+=512;
            len--;
        }
        ret=0;
    } else if (cmd==0x12) { //inquiry
        printf("HD: Inquiry\n");
        memcpy(data->data, inq_resp, sizeof(inq_resp));
        return 95;
    } else if (cmd==0x25) { //read capacity
        int lbacnt=hd->size/512;
        data->data[0]=(lbacnt>>24);
        data->data[1]=(lbacnt>>16);
        data->data[2]=(lbacnt>>8);
        data->data[3]=(lbacnt>>0);
        data->data[4]=0;
        data->data[5]=0;
        data->data[6]=2; //512
        data->data[7]=0;
        ret=8;
        printf("HD: Read capacity (%d)\n", lbacnt);
    } else {
        printf("********** hdScsiCmd: unrecognized command %x\n", cmd);
    }
    data->cmd[0]=0; //status
    data->msg[0]=0;
    return ret;
}

SCSIDevice *hdCreate(char *file) {
    (void)file;
    SCSIDevice *ret=(SCSIDevice*)malloc(sizeof(SCSIDevice));
    memset(ret, 0, sizeof(SCSIDevice));
    HdPriv *hd=(HdPriv*)malloc(sizeof(HdPriv));
    memset(hd, 0, sizeof(HdPriv));
    hd->part=esp_partition_find_first((esp_partition_type_t)0x40, (esp_partition_subtype_t)0x02, NULL);
    if (hd->part==0) {
        printf("Couldn't find HD partition!\n");
    } else {
        hd->size=hd->part->size;
        printf("HD partition found: %d bytes\n", hd->size);
    }
    ret->arg=hd;
    ret->scsiCmd=hdScsiCmd;
    return ret;
}
