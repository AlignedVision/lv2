/*
static char rcsid[] = "$Id$";

*/
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <errno.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/tcp.h>
#include <linux/laser_api.h>
#include "BoardComm.h"
#include "AppCommon.h"
#include "TargetFind.h"
#include "Laser_lv2_If.h"
#include "Protocol.h"
#include "comm_loop.h"
#include "parse_data.h"

// Static prototypes for local functions

static int FindSensedTarget(struct lg_master *pLgMaster, int16_t startX, int16_t startY, int16_t step, int16_t *foundX, int16_t *foundY, uint32_t numPoints, uint32_t buf_size, uint8_t sense_threshold, uint8_t sense_num_hits);
static int SuperScanFindMatch(struct lg_master *pLgMaster, int16_t startX, int16_t startY, int16_t *foundX, int16_t *foundY, uint32_t numPoints, uint32_t numLines);

//  Local functions
/****************************************************************
 *
 *  isOutOfBounds()
 *
 *  Description:   Checks to make sure points don't go off the
 *                 grid.
 *  Arguments:     <point> (x or y coord to test)
 *                 <step>  (value used to increment x or y by)
 *                 <numPoints> (number of points that will be used
 *                 in target find search)
 *
 *  Returns:       -1 if <point> is out of range in either direction.
 *                  0 if <point> is safe.
 *
 ***************************************************************/
int isOutOfBounds(int16_t point, uint16_t step, uint32_t count)
{
    int16_t   endPoint;

    endPoint = (point - (step * count)) & kMaxUnsigned;
    syslog(LOG_NOTICE, "isOutOfBounds():  negative test point = %x", endPoint);
    if (endPoint <= kMinSigned)
      return(-1);
    endPoint = (point + (step * count)) & kMaxUnsigned;
    syslog(LOG_NOTICE, "isOutOfBounds():  positive test point = %x", endPoint);
    if (endPoint >= kMaxSigned)
      return(-1);
    return(0);
}

    
static int FindSensedTarget(struct lg_master *pLgMaster, int16_t startX, int16_t startY, int16_t step, int16_t *foundX, int16_t *foundY, uint32_t numPoints, uint32_t buf_size, uint8_t sense_threshold, uint8_t sense_num_hits)
{
    struct write_sense_cs_data    *pSenseFound;
    uint32_t                      low_index;
    uint32_t                      i;
    uint32_t                      found_count;
    int                           rc;

    pSenseFound = (struct write_sense_cs_data *)malloc(buf_size);
    if (!pSenseFound)
      {
	syslog(LOG_ERR,"FindSensedTarget: ERROR trying to malloc buffer");
	return(-1);
      }
    // Need to memset because typically the amount of sense data read in is much smaller than the actual
    // allocated size in the driver
    memset(pSenseFound, 0, buf_size);
    rc = read(pLgMaster->fd_lv2, (uint8_t *)pSenseFound, buf_size);
    if (rc < 0)
      {
	syslog(LOG_ERR,"FindSensedTarget: ERROR reading from LV2 device #%d, errno=%x", pLgMaster->fd_lv2, errno);
	free((uint8_t *)pSenseFound);
	return(-1);
      }
    // Analyze data, look for at least 3 hits
    found_count = 0;
    low_index = 0;
    for (i = 0; i < numPoints; i++)
      {
	syslog(LOG_NOTICE,"FindSensedTarget: sense data[%d]=%x", i, pSenseFound[i].sense_val);
	if (pSenseFound[i].sense_val <= sense_threshold)
	  {
	    syslog(LOG_NOTICE,"FindSensedTarget: found good sense data[%d]=%x", i, pSenseFound[i].sense_val);
	    found_count++;
	    if (pSenseFound[i].sense_val < pSenseFound[low_index].sense_val)
	      low_index = i;
	  }
      }
    if (found_count < sense_num_hits)
      {
	syslog(LOG_NOTICE,"FindSensedTarget: NO sense data found, match-count = %d", found_count);
	free(pSenseFound);
	return(-1);
      }
    // Lowest sense value wins
    // FIXME---PAH---NEED TO JUST SEND BACK POINTS FROM DRIVER
    *foundX = startX + (step * low_index);
    *foundY = startY + (step * low_index);
    free(pSenseFound);
    return(0);
}
/****************************************************************
 *
 *  CoarseScanFindMatch()
 *
 *  Description:   Reads sense buffer at end of each box scan to
 *                 see if a target is found.  Criteria is 3 points
 *                 minimum.
 *  Arguments:     <pLgMaster> (pointer to master structure)
 *                 <numPoints> (number of write-sense operations done
 *                             during this search)
 *                 <startX> <startY>  (x,y pair that started search)
 *                 <read_sense_sz>  (size of sense read buffer) 
 *
 *  Returns:       -1 if no target is found.
 *                  0 if target found; <*foundX> <*foundY>
 *                          (x,y pair where target is found at)
 *
 ***************************************************************/
int CoarseScanFindMatch(struct lg_master *pLgMaster, uint32_t numPoints, int16_t startX, int16_t startY, uint16_t step, int16_t *foundX, int16_t *foundY)
{
    int        rc;
    uint32_t   buf_size;
    
    *foundX = kMaxUnsigned;
    *foundY = kMaxUnsigned;

    // Set up buffer to read sense data into from driver.
    buf_size = numPoints * sizeof(struct write_sense_cs_data);  
    if ((buf_size == 0) || (buf_size > MAX_TF_BUFFER))
      {
	syslog(LOG_ERR,"CoarseScanFindMatch: ERROR invalid # Points=%d", buf_size);
	return(-1);
      }
    syslog(LOG_NOTICE,"CoarseScanFindMatch: Start XY=%x,%x; numPoints = %d", startX, startY, numPoints);

    rc = FindSensedTarget(pLgMaster, startX, startY, step, foundX, foundY, numPoints, buf_size, CS_SENSE_THRESHOLD, CS_SENSE_HITS);
    if (rc < 0)
      return(rc);
    syslog(LOG_NOTICE, "CoarseScanFindMatch: Found target XY = %x,%x", *foundX, *foundY);
    return(0);
}
	
/****************************************************************
 *
 *  CoarseScan()
 *
 *  Description:   Sends command to LV2 device driver to perform an expanding
 *                 box algorithm to search for a target around startX/startY
 *                 arguments.
 *  Arguments:     <pLgMaster> (pointer to master structure)
 *                 <startX> <startY>  (x,y pair that start search)
 *
 *  Returns:       -1 if no target is found.
 *                  0 if target found; <*foundX> <*foundY>
 *                          (x,y pair where target is found at)
 *
 ***************************************************************/
int CoarseScan(struct lg_master *pLgMaster, int16_t startX, int16_t startY,
	       int16_t *foundX, int16_t *foundY)
{
    struct lv2_sense_info   sense_data;
    struct lv2_xypoints     xydata;
    //    int                     ret;
    uint32_t                box_loop_count;
    int16_t                 currentX=startX;
    int16_t                 currentY=startY;
    
    *foundX = kMaxUnsigned;
    *foundY = kMaxUnsigned;

    // Set up starting point
    box_loop_count = 0;
    
    // Starting arguments to box algorithm
    sense_data.point_delay = DEFAULT_SENSE_DELAY; 
    sense_data.step = COARSE_SCAN_STEP;
    sense_data.numPoints = COARSE_SCAN_NUM_POINTS;

    // Set up starting x,y points
    currentX = startX - sense_data.step;
    currentY = startY - sense_data.step;
    sense_data.xData = currentX;
    sense_data.yData = currentY;


    // Move mirrors to starting XY in the dark on each loop to avoid ghost/tail
    xydata.xPoint = startX; 
    xydata.yPoint = startY; 
    lv_setpoints_dark(pLgMaster, (struct lv2_xypoints *)&xydata);
    usleep(250);

    // loop until out of grid points or target is found
    //    while (box_loop_count < COARSE_SCAN_MAX_LOOPS)
    while (box_loop_count < 10)
      {
	if (isOutOfBounds(sense_data.xData, sense_data.step, sense_data.numPoints))
	  {
	    syslog(LOG_NOTICE, "BOX_LOOP %d: OUT-OF-BOUNDS step=%d, data=%x, numPoints=%d",
		   box_loop_count, sense_data.step, sense_data.xData, sense_data.numPoints);
	    break;
	  }
	if (isOutOfBounds(sense_data.yData, sense_data.step, sense_data.numPoints))
	  {
	    syslog(LOG_NOTICE, "BOX_LOOP %d: OUT-OF-BOUNDS step=%d, data=%x, numPoints=%d",
		   box_loop_count, sense_data.step, sense_data.yData, sense_data.numPoints);
	    break;
	  }

	syslog(LOG_NOTICE, "BOX_LOOP %d: X,Y=%x,%x; step=%d, numPoints=%d",
	       box_loop_count, currentX, currentY, sense_data.step, sense_data.numPoints);
	lv_box_sense_cmd(pLgMaster, (struct lv2_sense_info *)&sense_data);
    
#if 0
	ret = CoarseScanFindMatch(pLgMaster, sense_data.numPoints, currentX, currentY, sense_data.step, foundX, foundY);
	if (ret == 0)
	  {
	    syslog(LOG_NOTICE, "CoarseScan() TARGET FOUND:  current XY = %x,%x; found XY =%x,%x; numLoops=%d",
		   currentX, currentY, *foundX, *foundY, box_loop_count);
	    return(0);
	  }
#endif
	// Adjust X & Y back to bottom left corner of box
	sense_data.numPoints += ((box_loop_count * 2) + 2);
	box_loop_count++;
	currentX = startX - (COARSE_SCAN_STEP * (box_loop_count - 1));
	currentY = startY - (COARSE_SCAN_STEP * (box_loop_count - 1));
	sense_data.xData = currentX;
	sense_data.yData = currentY;
      }

    syslog(LOG_NOTICE, "CoarseScan() END:  NO target found, numLoops=%d", box_loop_count);
    return(-1);
}

/****************************************************************
 *
 *  FindSuperScanCoords()
 *
 *  Description:   Sends command to LV2 device driver to perform a cross search
 *                 algorithm to find the startX, endX, and number of lines to
 *                 perform super-scan search on.
 *  Arguments:     <pLgMaster> (pointer to master structure)
 *                 <startX> <startY>  (x,y pair that start search)
 *
 *  Returns:       -1 if no target is found.
 *                  0 if target found; <*foundX> <*foundY>
 *                          (x,y pair where target is found at)
 *
 ***************************************************************/
int FindSuperScanCoords(struct lg_master *pLgMaster, int16_t startX, int16_t startY,
			int16_t *superScanX, int16_t *superScanY, uint32_t *numLines, uint32_t *numPoints)
{
    struct lv2_sense_info   sense_data;
    struct lv2_xypoints     xydata;
    struct write_sense_fssc_data    *pSenseFound;
    int                     rc;
    
    *superScanX = kMaxUnsigned;
    *superScanY = kMaxUnsigned;
    *numLines   = 0;
    *numPoints   = 0;

    // Sense Buffer is filled by driver
    pSenseFound = (struct write_sense_fssc_data *)malloc(sizeof(struct write_sense_fssc_data));
    if (!pSenseFound)
      {
	syslog(LOG_ERR,"FindSSCoords(): ERROR trying to malloc buffer");
	return(-1);
      }

    // Set up parameters for driver scan
    sense_data.step = FSSC_SCAN_STEP;
    sense_data.numPoints = FSSC_SCAN_NUMPOINTS;
   
    sense_data.xData = startX;
    sense_data.yData = startY;

    // First move mirrors to start x,y location
    xydata.xPoint = startX; 
    xydata.yPoint = startY; 
    lv_setpoints_dark(pLgMaster, (struct lv2_xypoints *)&xydata);
    usleep(250);

    lv_find_ss_coords_sense_cmd(pLgMaster, (struct lv2_sense_info *)&sense_data);
    rc = read(pLgMaster->fd_lv2, (uint8_t *)pSenseFound, sizeof(struct write_sense_fssc_data));
    if (rc < 0)
      {
	syslog(LOG_ERR,"FindSSCoords(): ERROR reading from LV2 device #%d, errno=%x", pLgMaster->fd_lv2, errno);
	free((uint8_t *)pSenseFound);
	return(-1);
      }
    if ((pSenseFound->LeftEndpoints.xPoint == 0xFFFF)
	&& (pSenseFound->LeftEndpoints.yPoint == 0xFFFF)
	&& (pSenseFound->RightEndpoints.xPoint == 0xFFFF)
	&& (pSenseFound->RightEndpoints.yPoint == 0xFFFF)
	&& (pSenseFound->TopEndpoints.xPoint == 0xFFFF)
	&& (pSenseFound->TopEndpoints.yPoint == 0xFFFF)
	&& (pSenseFound->BottomEndpoints.xPoint == 0xFFFF)
	&& (pSenseFound->BottomEndpoints.yPoint == 0xFFFF))
      {
	syslog(LOG_ERR, "SuperScan Coords not found for xy=%x,%x!", startX, startY);
	free((uint8_t *)pSenseFound);
	return(-1);
      }
    // Found coords for superscan, send back to caller
    *numLines = pSenseFound->TopEndpoints.yPoint - pSenseFound->BottomEndpoints.yPoint;
    *numPoints = pSenseFound->RightEndpoints.xPoint - pSenseFound->LeftEndpoints.xPoint;
    *superScanX = pSenseFound->BottomEndpoints.xPoint - (*numPoints/2);
    *superScanY = pSenseFound->BottomEndpoints.yPoint;
    free((uint8_t *)pSenseFound);
    return(0);
}

/****************************************************************
 *
 *  SuperScanFindMatch()
 *
 *  Description:   Reads sense buffer at end of each box scan to
 *                 see if a target is found.  Criteria is 5 points
 *                 minimum.  Threshold is <= 0x30
 *  Arguments:     <pLgMaster> (pointer to master structure)
 *                 <numPoints> (number of write-sense operations done
 *                             during this search)
 *                 <startX> <startY>  (x,y pair that started search)
 *                 <read_sense_sz>  (size of sense read buffer) 
 *
 *  Returns:       -1 if no target is found.
 *                  0 if target found; <*foundX> <*foundY>
 *                          (x,y pair where target is found at)
 *
 ***************************************************************/
static int SuperScanFindMatch(struct lg_master *pLgMaster, int16_t startX, int16_t startY, int16_t *foundX, int16_t *foundY, uint32_t numPoints, uint32_t numLines)
{
    int        rc;
    uint32_t   buf_size;
    
    *foundX = kMaxUnsigned;
    *foundY = kMaxUnsigned;

    // Set up buffer to read sense data into from driver.
    buf_size = numPoints * numLines * sizeof(struct write_sense_cs_data);  
    if ((buf_size == 0) || (buf_size > MAX_TF_BUFFER))
      {
	syslog(LOG_ERR,"SuperScanFindMatch: ERROR invalid # Points=%d", buf_size);
	return(-1);
      }

    rc = FindSensedTarget(pLgMaster, startX, startY, SUPER_SCAN_STEP, foundX, foundY, numPoints, buf_size, SS_SENSE_THRESHOLD, SS_SENSE_HITS);
    if (rc < 0)
      return(rc);
    syslog(LOG_NOTICE, "SuperScanFindMatch: Found target XY = %x,%x", *foundX, *foundY);
    return(0);
}
	
/****************************************************************
 *
 *  SuperScan()
 *
 *  Description:   Sends command to LV2 device driver to perform
 *                 multiple line search in a square grid based on
 *                 dimensions of potential target found during the
 *                 FindSuperScanCoords() operation.
 *                 algorithm to find target dimensions & saves to log.
 *  Arguments:     <pLgMaster> (pointer to master structure)
 *                 <startX> <startY>  (x,y pair that start search)
 *                 <numLines>         (number of lines to search)
 *                 <numPoints>        (number of points per line)
 *
 *  Returns:       -1 if no target is found.
 *                  0 if target found; <*foundX> <*foundY>
 *                          (x,y pair where target is found at)
 *
 *  Logs:           TBD logs will be saved for post-processing.
 *
 ***************************************************************/
int SuperScan(struct lg_master *pLgMaster, int16_t startX, int16_t startY,
	      uint32_t numLines, uint32_t numPoints, int16_t *foundX, int16_t *foundY)
{
    struct lv2_ss_sense_info   sense_data;
    struct lv2_xypoints        xydata;
    int                        rc;

    sense_data.point_delay = DEFAULT_SENSE_DELAY;    // FIXME---PAH---Can be eliminated or user-defined?
    sense_data.step        = SUPER_SCAN_STEP;
    sense_data.numPoints   = numPoints;
    sense_data.numLines    = numLines;
    sense_data.xData       = startX;
    sense_data.yData       = startY;

    // First move mirrors to start x,y location
    xydata.xPoint = startX; 
    xydata.yPoint = startY; 
    lv_setpoints_dark(pLgMaster, (struct lv2_xypoints *)&xydata);
    usleep(250);

    // Send super-scan command to driver
    lv_super_scan_sense_cmd(pLgMaster, (struct lv2_ss_sense_info *)&sense_data);
    
    // Sift through the data to find center of target
    rc = SuperScanFindMatch(pLgMaster, startX, startY, foundX, foundY, numPoints, numLines);
    if (rc < 0)
      {
	syslog(LOG_ERR,"SuperScan(): Target NOT found");
	return(rc);
      }
    
    syslog(LOG_NOTICE, "SuperScan() TARGET FOUND AT:  %x,%x", *foundX, *foundY);
    return(0);
}

int FindTarget(struct lg_master *pLgMaster, int16_t startX, int16_t startY,
	       int16_t *foundX, int16_t *foundY)
{
    struct lv2_xypoints  xyPoints;
    uint32_t             numPoints;
    uint32_t             numLines;
    int                  rc;

    rc = CoarseScan(pLgMaster, startX, startY, foundX, foundY);
    // Move mirrors to starting XY in the dark on each loop to avoid ghost/tail
    xyPoints.xPoint = startX; 
    xyPoints.yPoint = startY; 
    lv_setpoints_dark(pLgMaster, (struct lv2_xypoints *)&xyPoints);
    if (rc)
      return(rc);
    if ((*foundX == kMaxUnsigned) && (*foundY == kMaxUnsigned))
      return(-1);

    // Got a target
    xyPoints.xPoint = *foundX;
    xyPoints.yPoint = *foundY;
    syslog(LOG_NOTICE, "CoarseScan(): Found target at x=%x,y=%x", *foundX, *foundY);

    // Next phase, find endpoints for super-fine scan
    startX = *foundX;
    startY = *foundY;
    rc = FindSuperScanCoords(pLgMaster, startX, startY, foundX, foundY, &numLines, &numPoints);
    if (rc)
      return(rc);
    if ((*foundX == kMaxUnsigned) && (*foundY == kMaxUnsigned))
      {
	syslog(LOG_ERR, "FindSSCoords(): Target not found for XY=%x,%x", startX, startY);
	return(-1);
      }
    if ((numLines == 0) || (numPoints == 0))
      {
	syslog(LOG_ERR, "FindSSCoords(): numPoints & numLines = 0 for XY=%x,%x, foundXY=%x,%x", startX, startY, *foundX, *foundY);
	return(-1);
      }
	
    syslog(LOG_NOTICE, "FindSSCoords(): Found target at x=%x,y=%x, numPoints=%d, numLines=%d", *foundX, *foundY, numPoints, numLines);
    startX = *foundX;
    startY = *foundY;
    rc = SuperScan(pLgMaster, startX, startY, numLines, numPoints, foundX, foundY);
    if (rc)
      {
	syslog(LOG_NOTICE, "TargetFind(): TARGET NOT FOUND");
	return(rc);
      }
    startX = *foundX;
    startY = *foundY;
    syslog(LOG_NOTICE, "TargetFind(): Found target at XY = %x,%x", *foundX, *foundY);
    return(0);
}
