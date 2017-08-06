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
#include "Laser_If.h"
#include "Protocol.h"
#include "comm_loop.h"
#include "parse_data.h"

// Static prototypes for local functions

static int isOutOfBounds(int16_t point, int16_t step, uint16_t count);
static int CoarseScanFindMatch(struct lg_master *pLgMaster, uint32_t line_count, struct write_sense_data *pFoundTarget);

//  Local functions

int isOutOfBounds(int16_t point, int16_t step, uint16_t count)
{
  if ((point - (step * count)) <= kMinSigned)
    return(-1);
  if ((point + (step * count)) >= kMaxSigned)
    return(-1);
  
  return(0);
}
int CoarseScanFindMatch(struct lg_master *pLgMaster, uint32_t line_count, struct write_sense_data *pFoundTarget)
{
    struct write_sense_data    *pSenseFound;
    int        rc;
    uint32_t   data_size;
    uint32_t   low_index;
    uint16_t   i;
    uint8_t   found_count;
    
    data_size = line_count * sizeof(struct write_sense_data);
    syslog(LOG_NOTICE,"CoarseScanFindMatch: count=%d,data_size=%d", line_count, data_size);

    if (data_size > MAX_TGFIND_BUFFER)
      {
	syslog(LOG_ERR, "CoarseScanFindMatch: length %d too long for sensing, max=%ld", data_size, MAX_TGFIND_BUFFER);
	return(-1);
      }
    // Sense Buffer is filled by driver
    pSenseFound = (struct write_sense_data *)malloc(data_size);
    if (!pSenseFound)
      {
	syslog(LOG_ERR,"CoarseScanFindMatch: ERROR trying to malloc buffer");
	return(-1);
      }
    // Need to memset because typically the amount of sense data read in is much smaller than the actual
    // allocated size in the driver
    memset(pSenseFound, 0, data_size);
    rc = read(pLgMaster->fd_lv2, (uint8_t *)pSenseFound, data_size);
    if (rc < 0)
      {
	syslog(LOG_ERR,"CoarseScanFindMatch: ERROR reading from LV2 device #%d", pLgMaster->fd_lv2);
	free((uint8_t *)pSenseFound);
	return(-1);
      }
    // Analyze data, look for at least 3 hits
    found_count = 0;
    low_index = 0;
    for (i = 0; i < line_count; i++)
      {
	if (pSenseFound[i].sense_val <= COARSE_SCAN_THRESHOLD2)
	  {
	    syslog(LOG_NOTICE,"CoarseScanFindMatch: found good sense data[%d]=%x", i, pSenseFound[i].sense_val);
	    found_count++;
	    if (pSenseFound[i].sense_val < pSenseFound[low_index].sense_val)
	      low_index = i;
	  }
      }
    if (found_count < COARSE_SCAN_MIN_FOUND)
      {
	syslog(LOG_NOTICE,"CoarseScanFindMatch: NO sense data found");
	free(pSenseFound);
	pFoundTarget->sense_val = 0xFF;
	pFoundTarget->point = kMaxUnsigned;
	return(0);
      }
    // Lowest sense value wins
    pFoundTarget->sense_val = pSenseFound[low_index].sense_val;
    pFoundTarget->point = pSenseFound[low_index].point;
    free(pSenseFound);
    return(0);
}
int CoarseScan_loop(struct lg_master *pLgMaster, int16_t startX, int16_t startY,
	       int16_t *foundX, int16_t *foundY)
{
    struct lv2_sense_info   sense_data;
    struct lv2_xypoints     xydata;
    struct write_sense_data found_target;
    int                     ret;
    uint32_t                box_loop_count;
    int16_t                 currentX;
    int16_t                 currentY;
    
    *foundX = kMaxUnsigned;
    *foundY = kMaxUnsigned;

    // Set up starting point
    sense_data.step = COARSE_SCAN_STEP;
    sense_data.loop_count = COARSE_SCAN_COUNT;
    
    box_loop_count = 0;
    syslog(LOG_NOTICE,"STARTING BOX FUNCTION, X=%x,Y=%x", startX, startY);

    // Move mirrors to starting XY in the dark to avoid ghost/tail
    xydata.xPoint = startX; 
    xydata.xPoint = startY; 
    lv_setpoints_dark(pLgMaster, (struct lv2_xypoints *)&xydata);
    usleep(100);
	
    while (1)
      {
	// Adjust X & Y to start at bottom left corner of box
	currentX = startX - (abs(sense_data.step) * (box_loop_count + 1));
	currentY = startY - (abs(sense_data.step) * (box_loop_count + 1));
	sense_data.data = currentY;
	if (isOutOfBounds(sense_data.data, sense_data.step, sense_data.loop_count))
	  {
	    syslog(LOG_NOTICE, "BOX_LOOP: OUT-OF-BOUND step=%d, data=%x, loop_count=%d",
		   sense_data.step, sense_data.data, sense_data.loop_count);
	    return(-1);
	  }
	syslog(LOG_NOTICE,"BOX LOOP, currentX=%x,currentY=%x", currentX, currentY);
	// Box Left side go up, X step 0, Y + step
	if (box_loop_count > COARSE_SCAN_MAX_LOOPS)
	  {
	    if (box_loop_count > COARSE_SCAN_MAX_LOOPS)
	      syslog(LOG_ERR, "COARSE_SCAN: Not found in %d loops", COARSE_SCAN_MAX_LOOPS);
	    else	      
	      syslog(LOG_ERR, "COARSE_SCAN: out-of-bounds condition for pt %x, step %d, count %d",
		     sense_data.data, sense_data.step, sense_data.loop_count);
	    return(-1);
	  }
	// Search UP left side of box (move Y +step)
	syslog(LOG_NOTICE,"BOX UP-LEFT SIDE: SenseY, senseY=%x, X=%x", sense_data.data, currentX);
	lv_senseY_cmd(pLgMaster, (struct lv2_sense_info *)&sense_data);
	ret = CoarseScanFindMatch(pLgMaster, sense_data.loop_count, &found_target);
	if (ret == 0)
	  {
	    if (found_target.sense_val < COARSE_SCAN_THRESHOLD2)
	      {
		*foundX = currentX;
		*foundY = found_target.point;
		syslog(LOG_NOTICE, "found x=%x; found y=%x",*foundX, *foundY);
		return(0);
	      }
	  }
	// Adjust X & Y to top left corner of box
	currentY += abs(sense_data.step) * sense_data.loop_count;
	sense_data.data = currentX;

	if (isOutOfBounds(sense_data.data, sense_data.step, sense_data.loop_count))
	  {
	    syslog(LOG_NOTICE, "BOX_LOOP: OUT-OF-BOUND step=%d, data=%x, loop_count=%d",
		   sense_data.step, sense_data.data, sense_data.loop_count);
	    return(-1);
	  }
	// Search ACROSS-RIGHT top of box (move X, +step)
	syslog(LOG_NOTICE,"BOX ACROSS-TOP-RIGHT:  SenseX, senseX=%x, Y=%x", sense_data.data, currentY);
	lv_senseX_cmd(pLgMaster, (struct lv2_sense_info *)&sense_data);
	ret = CoarseScanFindMatch(pLgMaster, sense_data.loop_count, &found_target);
	if (ret == 0)
	  {
	    if (found_target.sense_val < COARSE_SCAN_THRESHOLD2)
	      {
		*foundX = found_target.point;
		*foundY = currentY;
		syslog(LOG_NOTICE, "found x=%x; found y=%x",*foundX, *foundY);
		return(0);
	      }
	  }
	// Adjust X & Y to top right corner of box
	currentY += abs(sense_data.step) * sense_data.loop_count;
	sense_data.data = currentY;

	// Search DOWN right side of box (move Y ~step)
	sense_data.step = ~sense_data.step;
	if (isOutOfBounds(sense_data.data, sense_data.step, sense_data.loop_count))
	  {
	    syslog(LOG_NOTICE, "BOX_LOOP: OUT-OF-BOUND step=%d, data=%x, loop_count=%d",
		   sense_data.step, sense_data.data, sense_data.loop_count);
	    return(-1);
	  }
	syslog(LOG_NOTICE,"BOX DOWN-RIGHT SIDE: SenseY, senseY=%x, X=%x", sense_data.data, currentX);
	lv_senseY_cmd(pLgMaster, (struct lv2_sense_info *)&sense_data);
	ret = CoarseScanFindMatch(pLgMaster, sense_data.loop_count, &found_target);
	if (ret == 0)
	  {
	    if (found_target.sense_val < COARSE_SCAN_THRESHOLD2)
	      {
		*foundX = currentX;
		*foundY = found_target.point;
		syslog(LOG_NOTICE, "found x=%x; found y=%x",*foundX, *foundY);
		return(0);
	      }
	  }
	// Adjust X & Y to bottom right corner.
	currentX -= abs(sense_data.step) * sense_data.loop_count;
	sense_data.data = currentX;
	if (isOutOfBounds(sense_data.data, sense_data.step, sense_data.loop_count))
	  {
	    syslog(LOG_NOTICE, "BOX_LOOP: OUT-OF-BOUND step=%d, data=%x, loop_count=%d",
		   sense_data.step, sense_data.data, sense_data.loop_count);
	    return(-1);
	  }

	// Search ACROSS LEFT bottom of box (move X, ~step)
	syslog(LOG_NOTICE,"BOX ACROSS-BOTTOM-LEFT:  SenseX, senseX=%x, Y=%x", sense_data.data, currentY);
	lv_senseX_cmd(pLgMaster, (struct lv2_sense_info *)&sense_data);
	ret = CoarseScanFindMatch(pLgMaster, sense_data.loop_count, &found_target);
	if (ret == 0)
	  {
	    if (found_target.sense_val < COARSE_SCAN_THRESHOLD2)
	      {
		*foundX = found_target.point;
		*foundY = currentY;
		syslog(LOG_NOTICE, "found x=%x; found y=%x",*foundX, *foundY);
		return(0);
	      }
	  }

	box_loop_count++;
	sense_data.loop_count += 2;
      }
    return(-1);
}
int CoarseScan_loop_single(struct lg_master *pLgMaster, int16_t startX, int16_t startY,
	       int16_t *foundX, int16_t *foundY)
{
    struct lv2_sense_one_info   sense_data;
    struct lv2_xypoints         xydata;
    struct write_sense_data     found_target;
    int                         ret;
    uint32_t                    max_num_boxes;
    uint32_t                    line_point_count;
    uint32_t                    line_step;
    int16_t                     currentX;
    int16_t                     currentY;
    uint16_t                    i;
    
    *foundX = kMaxUnsigned;
    *foundY = kMaxUnsigned;

    // Set up starting point
    line_step = COARSE_SCAN_STEP;
    line_point_count = COARSE_SCAN_COUNT;
    
    max_num_boxes = 0;
    syslog(LOG_NOTICE,"STARTING BOX FUNCTION, X=%x,Y=%x", startX, startY);

    // Move mirrors to starting XY in the dark to avoid ghost/tail
    xydata.xPoint = startX; 
    xydata.yPoint = startY; 
    lv_setpoints_dark(pLgMaster, (struct lv2_xypoints *)&xydata);
    usleep(100);
	
    while (1)
      {
	// Adjust X & Y to start at bottom left corner of box
	currentX = startX - (abs(line_step) * (max_num_boxes + 1));
	currentY = startY - (abs(line_step) * (max_num_boxes + 1));
	sense_data.data = currentY;
	if (isOutOfBounds(sense_data.data, line_step, line_point_count))
	  {
	    syslog(LOG_NOTICE, "BOX_LOOP: OUT-OF-BOUND step=%d, data=%x, loop_count=%d",
		   line_step, sense_data.data, line_point_count);
	    return(-1);
	  }
	syslog(LOG_NOTICE,"BOX LOOP, currentX=%x,currentY=%x", currentX, currentY);
	// Make sure we haven't exceeded max # of boxes.
	if (max_num_boxes > COARSE_SCAN_MAX_LOOPS)
	  {
	    if (max_num_boxes > COARSE_SCAN_MAX_LOOPS)
	      syslog(LOG_ERR, "COARSE_SCAN: Not found in %d loops", COARSE_SCAN_MAX_LOOPS);
	    else	      
	      syslog(LOG_ERR, "COARSE_SCAN: out-of-bounds condition for pt %x, step %d, count %d",
		     sense_data.data, line_step, line_point_count);
	    return(-1);
	  }
	// Search UP left side of box (move Y +step)
	for (i = 0; i < line_point_count; i++)
	  {
	    sense_data.data += line_step * i;
	    sense_data.index = i;
	    syslog(LOG_NOTICE,"BOX UP-LEFT SIDE: SenseY, senseY=%x, X=%x", sense_data.data, currentX);
	    lv_sense_oneY_cmd(pLgMaster, (struct lv2_sense_one_info *)&sense_data);
	  }
	ret = CoarseScanFindMatch(pLgMaster, line_point_count, &found_target);
	if (ret == 0)
	  {
	    if (found_target.sense_val < COARSE_SCAN_THRESHOLD2)
	      {
		*foundX = currentX;
		*foundY = found_target.point;
		syslog(LOG_NOTICE, "found x=%x; found y=%x",*foundX, *foundY);
		return(0);
	      }
	  }
	// Adjust X & Y to top left corner of box
	currentY += abs(line_step) * line_point_count;
	sense_data.data = currentX;

	if (isOutOfBounds(sense_data.data, line_step, line_point_count))
	  {
	    syslog(LOG_NOTICE, "BOX_LOOP: OUT-OF-BOUND step=%d, data=%x, loop_count=%d",
		   line_step, sense_data.data, line_point_count);
	    return(-1);
	  }
	// Search ACROSS-RIGHT top of box (move X, +step)
	for (i = 0; i < line_point_count; i++)
	  {
	    sense_data.data += line_step * i;
	    sense_data.index = i;
	    syslog(LOG_NOTICE,"BOX ACROSS-TOP-RIGHT:  SenseX, senseX=%x, Y=%x", sense_data.data, currentY);
	    lv_sense_oneX_cmd(pLgMaster, (struct lv2_sense_one_info *)&sense_data);
	  }
	ret = CoarseScanFindMatch(pLgMaster, line_point_count, &found_target);
	if (ret == 0)
	  {
	    if (found_target.sense_val < COARSE_SCAN_THRESHOLD2)
	      {
		*foundX = found_target.point;
		*foundY = currentY;
		syslog(LOG_NOTICE, "found x=%x; found y=%x",*foundX, *foundY);
		return(0);
	      }
	  }
	// Adjust X & Y to top right corner of box
	currentY += abs(line_step) * line_point_count;
	sense_data.data = currentY;

	// Search DOWN right side of box (move Y ~step)
	line_step = ~line_step;
	if (isOutOfBounds(sense_data.data, line_step, line_point_count))
	  {
	    syslog(LOG_NOTICE, "BOX_LOOP: OUT-OF-BOUND step=%d, data=%x, loop_count=%d",
		   line_step, sense_data.data, line_point_count);
	    return(-1);
	  }
	for (i = 0; i < line_point_count; i++)
	  {
	    sense_data.data += line_step * i;
	    sense_data.index = i;
	    syslog(LOG_NOTICE,"BOX DOWN-RIGHT SIDE: SenseY, senseY=%x, X=%x", sense_data.data, currentX);
	    lv_sense_oneY_cmd(pLgMaster, (struct lv2_sense_one_info *)&sense_data);
	  }
	ret = CoarseScanFindMatch(pLgMaster, line_point_count, &found_target);
	if (ret == 0)
	  {
	    if (found_target.sense_val < COARSE_SCAN_THRESHOLD2)
	      {
		*foundX = currentX;
		*foundY = found_target.point;
		syslog(LOG_NOTICE, "found x=%x; found y=%x",*foundX, *foundY);
		return(0);
	      }
	  }
	// Adjust X & Y to bottom right corner.
	currentX -= abs(line_step) * line_point_count;
	sense_data.data = currentX;
	if (isOutOfBounds(sense_data.data, line_step, line_point_count))
	  {
	    syslog(LOG_NOTICE, "BOX_LOOP: OUT-OF-BOUND step=%d, data=%x, loop_count=%d",
		   line_step, sense_data.data, line_point_count);
	    return(-1);
	  }

	// Search ACROSS LEFT bottom of box (move X, ~step)
	for (i = 0; i < line_point_count; i++)
	  {
	    sense_data.data += line_step * i;
	    sense_data.index = i;
	    syslog(LOG_NOTICE,"BOX ACROSS-BOTTOM-LEFT:  SenseX, senseX=%x, Y=%x", sense_data.data, currentY);
	    lv_sense_oneX_cmd(pLgMaster, (struct lv2_sense_one_info *)&sense_data);
	  }
	ret = CoarseScanFindMatch(pLgMaster, line_point_count, &found_target);
	if (ret == 0)
	  {
	    if (found_target.sense_val < COARSE_SCAN_THRESHOLD2)
	      {
		*foundX = found_target.point;
		*foundY = currentY;
		syslog(LOG_NOTICE, "found x=%x; found y=%x",*foundX, *foundY);
		return(0);
	      }
	  }

	max_num_boxes++;
	line_point_count += 2;
      }
    return(-1);
}
int FindSuperScanCoords(struct lg_master *pLgMaster, int16_t startX, int16_t startY,
		      int16_t *foundX, int16_t *foundY)
{
    // FIXME---PAH---TBD
    return(0);
}
int SuperScan(struct lg_master *pLgMaster, int16_t startX, int16_t startY,
		      int16_t *foundX, int16_t *foundY)
{
    // FIXME---PAH---TBD
    return(0);
}
int ScanEnd(struct lg_master *pLgMaster, int16_t startX, int16_t startY,
		      int16_t *foundX, int16_t *foundY)
{
    // FIXME---PAH---TBD
    // show targets, write out log files
    return(0);
}
    
int FindTarget(struct lg_master *pLgMaster, int16_t startX, int16_t startY,
	       int16_t *foundX, int16_t *foundY)
{
    struct   lv2_xypoints  xyPoints;
    int                    rc;

    rc = CoarseScan_loop_single(pLgMaster, startX, startY, foundX, foundY);
    if (rc)
      return(rc);
    startX = *foundX;
    startY = *foundY;
    xyPoints.xPoint = *foundX;
    xyPoints.yPoint = *foundY;
    syslog(LOG_NOTICE, "Found target at x=%x,y=%x", *foundX, *foundY);
    // FIXME---PAH---TEMP BLINK FOR INDICATOR WE FOUND SOMETHING
    lv_setpoints_lite(pLgMaster, (struct lv2_xypoints *)&xyPoints);
    sleep(1);
    lv_setpoints_dark(pLgMaster, (struct lv2_xypoints *)&xyPoints);
    sleep(1);
    lv_setpoints_lite(pLgMaster, (struct lv2_xypoints *)&xyPoints);

    rc = FindSuperScanCoords(pLgMaster, startX, startY, foundX, foundY);
    if (rc)
      return(rc);
    startX = *foundX;
    startY = *foundY;
    rc = SuperScan(pLgMaster, startX, startY, foundX, foundY);
    if (rc)
      return(rc);
    startX = *foundX;
    startY = *foundY;
    rc = ScanEnd(pLgMaster, startX, startY, foundX, foundY);
    return(rc);
}