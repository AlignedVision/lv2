#include <stdint.h>

#ifndef TARGETFIND_H
#define TARGETFIND_H


#define SENSE_BUF_SIZE          sizeof(int16_t) * 500000
#define COARSE_SCAN_STEP        10
#define COARSE_SCAN_COUNT       10
#define COARSE_SCAN_SENSE_DELAY 40
#define COARSE_SCAN_MIN_FOUND    5
#define COARSE_SCAN_MAX_LOOPS   70   // 4" square, FIXME---PAH---SHOULD BE ENTERED BY USER EVENTUALLY
#define COARSE_SCAN_THRESHOLD1  60   // FIXME---PAH---SHOULD BE ENTERED BY USER EVENTUALLY
#define COARSE_SCAN_THRESHOLD2  60   // FIXME---PAH---SHOULD BE ENTERED BY USER EVENTUALLY

int FindSuperScanCoords(struct lg_master *pLgMaster, int16_t startX, int16_t startY,
			int16_t *foundX, int16_t *foundY);
int CoarseScan_drv(struct lg_master *pLgMaster, int16_t startX, int16_t startY,
		   int16_t *foundX, int16_t *foundY);
int CoarseScan(struct lg_master *pLgMaster, int16_t startX, int16_t startY,
		      int16_t *foundX, int16_t *foundY);
int SuperScan(struct lg_master *pLgMaster, int16_t startX, int16_t startY,
	      int16_t *foundX, int16_t *foundY);
int ScanEnd(struct lg_master *pLgMaster, int16_t startX, int16_t startY,
	    int16_t *foundX, int16_t *foundY);
int FindTarget(struct lg_master *pLgMaster, int16_t startX, int16_t startY,
	       int16_t *foundX, int16_t *foundY);

#endif
