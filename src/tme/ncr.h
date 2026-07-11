#ifndef NCR_H
#define NCR_H
#include <stdint.h>

#define SCSI_BUS_ID_COUNT 8
#define SCSI_INITIATOR_ID 7
#define SCSI_TARGET_COUNT SCSI_INITIATOR_ID
#define SCSI_DATA_BUFFER_SIZE (128*1024)

typedef struct {
	uint8_t cmd[256];
	uint8_t *data;
	uint8_t msg[128];
	int cmdlen;
	int datalen;
	int msglen;
} SCSITransferData;

typedef struct {
	int (*scsiCmd)(SCSITransferData *data, unsigned int cmd, unsigned int len, unsigned int lba, void *arg);
	void *arg;
} SCSIDevice;


void ncrInit();
void ncrRegisterDevice(int id, SCSIDevice* dev);
unsigned int ncrRead(unsigned int addr, unsigned int dack);
void ncrWrite(unsigned int addr,unsigned int dack, unsigned int val);

#endif
