/* -*- C -*-
  Copyright (C) 2009 Thomas Schmitt <scdbackup@gmx.net>
  Copyright (C) 2010 Rocky Bernstein <rocky@gnu.org>

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/* 
   Regression test for MMC commands involving read/write access.
*/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif
#include <cdio/cdio.h>
#include <cdio/logging.h>
#include <cdio/mmc_cmds.h>

#ifdef HAVE_STDIO_H
#include <stdio.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#include <string.h>

#define SKIP_TEST 77

/* The compiler warns if no prototypes are given before function definition */

static int test_eject_load_cycle(CdIo_t *p_cdio, unsigned int i_flag);

static int test_eject_test_load(CdIo_t *p_cdio, unsigned int i_flag);

static int test_handle_outcome(CdIo_t *p_cdio, int i_status,
			       int *sense_avail, 
			       unsigned char sense_reply[18], 
			       unsigned int i_flag);

static void test_print_status_sense(int i_status, int sense_valid,
				    cdio_mmc_request_sense_t *, 
				    unsigned int i_flag);

static int test_load_eject(CdIo_t *p_cdio, int *sense_avail,
			   unsigned char sense_reply[18], unsigned int i_flag);
static int test_mode_select(CdIo_t *p_cdio,
			    int *sense_avail, unsigned char sense_reply[18],
			    unsigned char *p_buf, unsigned int i_size, 
			    unsigned int i_flag);

static int test_mode_sense(CdIo_t *p_cdio, int *sense_avail,
			   unsigned char sense_reply[18],
			   int page_code, int subpage_code, int alloc_len,
			   unsigned char *buf, int *i_size, 
			   unsigned int i_flag);
static int test_test_unit_ready(CdIo_t *p_cdio, int *sense_avail,
				unsigned char sense_reply[18], 
				unsigned int i_flag);
static int test_wait_for_drive(CdIo_t *p_cdio, int max_tries, 
			       unsigned int i_flag);

static int test_rwr_mode_page(CdIo_t *p_cdio, unsigned int i_flag);

static int test_write(char *drive_path, unsigned int i_flag);


/* ------------------------- Helper functions ---------------------------- */


/* @param flag bit0= verbose
*/
static void
test_print_status_sense(int i_status, int sense_valid,
                        cdio_mmc_request_sense_t *p_sense_reply, 
			unsigned int i_flag)
{
    if (!(i_flag & 1))
	return;
    fprintf(stderr, "return= %d , sense(%d)", i_status, sense_valid);
    if (sense_valid >= 14)
	fprintf(stderr, ":  KEY=%s (%1.1X), ASC= %2.2X , ASCQ= %2.2X",
		mmc_sense_key2str[p_sense_reply->sense_key],
		p_sense_reply->sense_key,
		p_sense_reply->asc, 
		p_sense_reply->ascq);
    fprintf(stderr, "\n");  
}


/* @param flag         bit0= verbose
*/
static int
test_handle_outcome(CdIo_t *p_cdio, int i_status,
                    int *sense_avail, unsigned char sense_reply[18], 
		    unsigned int i_flag)
{
  cdio_mmc_request_sense_t *p_sense = NULL;

  *sense_avail = mmc_last_cmd_sense(p_cdio, &p_sense);
  test_print_status_sense(i_status, *sense_avail, p_sense, i_flag & 1);
  if (*sense_avail >= 18)
    memcpy(sense_reply, p_sense, 18);
  else
    memset(sense_reply, 0, 18);
  if (p_sense != NULL)
    free(p_sense);
  return i_status;
}


/* --------------------------- MMC commands ------------------------------ */


/* UNOBTRUSIVE */
/* @param flag         bit0= verbose
   @param sense_avail  Number of available sense bytes
                       (18 get copied if all 18 exist)
   @param sense_reply  eventual sense bytes
   @return             return value of mmc_run_cmd()
*/
static int
test_test_unit_ready(CdIo_t *p_cdio,
                     int *sense_avail, unsigned char sense_reply[18], 
		     unsigned int i_flag)
{
  int i_status;

  if (i_flag & 1)
    fprintf(stderr, "test_test_unit_ready ... ");
  i_status = mmc_test_unit_ready(p_cdio, 0);

  return test_handle_outcome(p_cdio, i_status, sense_avail, sense_reply,
                             i_flag & 1);
}


/* OBTRUSIVE. PHYSICAL EFFECT: DANGER OF HUMAN INJURY */
/* @param i_flag bit0= verbose
                 bit1= Asynchronous operation
                 bit2= Load (else Eject)
   @return      return value of mmc_run_cmd()
*/
static int
test_load_eject(CdIo_t *p_cdio, int *sense_avail,
                unsigned char sense_reply[18], unsigned int i_flag)
{
  int i_status;
  bool b_eject = !!(i_flag & 4);
  bool b_immediate = !!(i_flag & 2);

  i_status = mmc_start_stop_unit(p_cdio, b_eject, b_immediate, 0, 0);

  if (i_flag & 1)
    fprintf(stderr, "test_load_eject(0x%X) ... ", i_flag);

  return test_handle_outcome(p_cdio, i_status, sense_avail, sense_reply,
                             i_flag & 1);
}


/* BARELY OBTRUSIVE: MIGHT SPOIL BURN RUNS */
/* Fetch a mode page or a part of it from the drive.
   @param alloc_len  The number of bytes to be requested from the drive and to
                     be copied into parameter buf.
                     This has to include the 8 bytes of header and may not
                     be less than 10.
   @param buf        Will contain at most alloc_len many bytes. The first 8 are
                     a Mode Parameter Header as of SPC-3 7.4.3, table 240.
                     The further bytes are the mode page, typically as of
                     MMC-5 7.2. There are meanwhile deprecated mode pages which
                     appear only in older versions of MMC.
   @param i_size     Will return the number of actually read bytes resp. the
                     number of available bytes. See flag bit1.
   @param flag       bit0= verbose
                     bit1= Peek mode:
                           Reply number of available bytes in *i_size and not
                           the number of actually read bytes.
   @return           return value of mmc_run_cmd(),
                     or other driver_return_code_t
*/
static driver_return_code_t
test_mode_sense(CdIo_t *p_cdio, int *sense_avail,
		unsigned char sense_reply[18],
                int page_code, int subpage_code, int alloc_len,
                unsigned char *buf, int *i_size, unsigned int i_flag)
{
  driver_return_code_t i_status;
  
  if (alloc_len < 10)
    return DRIVER_OP_BAD_PARAMETER;

  if (i_flag & 1)
    fprintf(stderr, "test_mode_sense(0x%X, %X, %d) ... ",
	    (unsigned int) page_code, (unsigned int) subpage_code, alloc_len);
  
  i_status = mmc_mode_sense_10(p_cdio, buf, alloc_len, page_code);
  test_handle_outcome(p_cdio, i_status, sense_avail, sense_reply, i_flag & 1);
  if (DRIVER_OP_SUCCESS != i_status)
    return i_status;
  if (i_flag & 2)
    *i_size = buf[9] + 10;                /* MMC-5 7.2.3 */
  else
    *i_size = buf[0] * 256 + buf[1] + 2;  /* SPC-3 7.4.3, table 240 */
  return i_status;
}


/* OBTRUSIVE. SPOILS BURN RUNS and might return minor failure with
   -ROM drives */
/* Send a mode page to the drive.
   @param buf        Contains the payload bytes. The first 8 shall be a Mode
                     Parameter Header as of SPC-3 7.4.3, table 240.
                     The further bytes are the mode page, typically as of
                     MMC-5 7.2. There are meanwhile deprecated mode pages which
                     appear only in older versions of MMC.
   @param i_size   The number of bytes in buf.
   @param flag       bit0= verbose
   @return           return value of mmc_run_cmd(),
                     or other driver_return_code_t
*/
static int
test_mode_select(CdIo_t *p_cdio,
                 int *sense_avail, unsigned char sense_reply[18],
                 unsigned char *p_buf, unsigned int i_size, unsigned int i_flag)
{
    int i_status, i;

    if (i_size < 10)
	return DRIVER_OP_BAD_PARAMETER;
    
    if (i_flag & 1) {
	printf("test_mode_select to drive: %d bytes\n", i_size);
	for (i = 0; i < i_size; i++) {
	    printf("%2.2X ", (unsigned int) p_buf[i]);
	    if ((i % 20) == 19)
		printf("\n");
	}
	if ((i % 20))
	    printf("\n");
    }
    
    if (i_flag & 1)
	fprintf(stderr, "test_mode_select(0x%X, %d, %d) ... ",
		(unsigned int) p_buf[8], (unsigned int) p_buf[9], i_size);
    i_status = mmc_mode_select_10(p_cdio, p_buf, i_size, 0x10, 10000);
    return test_handle_outcome(p_cdio, i_status, sense_avail, sense_reply,
			       i_flag & 1);
}

/* --------------------------- Larger gestures ----------------------------- */


/* UNOBTRUSIVE */
/* Watch drive by test unit ready loop until ready, no media or timeout.
   @param flag bit0= verbose
               bit1= expect media (do not end on no-media sense)
   @return     1= all seems well , 0= minor failure , -1= severe failure
*/
static int
test_wait_for_drive(CdIo_t *p_cdio, int max_tries, unsigned int i_flag)
{
  int ret, i, sense_avail;
  unsigned char sense_reply[18];
  cdio_mmc_request_sense_t *p_sense_reply = 
      (cdio_mmc_request_sense_t *) sense_reply;
  
  for (i = 0; i < max_tries; i++) {
    ret = test_test_unit_ready(p_cdio, &sense_avail, sense_reply, i_flag & 1);
    if (ret == 0) /* Unit is ready */
      return 1;
    if (sense_avail < 18)
      return -1;
    if (p_sense_reply->sense_key == 2 && sense_reply[12] == 0x04) {

      /* Not ready */;

    } else if (p_sense_reply->sense_key == 6 && sense_reply[12] == 0x28
               && sense_reply[13] == 0x00) {

      /* Media change notice = try again */;

    } else if (p_sense_reply->sense_key == 2 && sense_reply[12] == 0x3a) {

      /* Medium not present */;

      if (!(i_flag & 2))
        return 1;
    } else if (p_sense_reply->sense_key == 0 && sense_reply[12] == 0) {

      /* Error with no sense */;

      return -1;
  break;
    } else {

      /* Other error */;

      return 0;
    } 
    sleep(1);
  }
  fprintf(stderr, "test_eject_load_cycle: Drive not ready after %d retries\n",
          max_tries);
  return -1;
}


/* OBTRUSIVE , PHYSICAL EFFECT , DANGER OF HUMAN INJURY */
/* Eject, wait, load asynchronously, and watch by test unit ready loop.
   @param i_flag bit0= verbose
                 bit1= expect media (do not end on no-media sense)
   @return   1= all seems well , 0= minor failure , -1= severe failure
*/
static int
test_eject_load_cycle(CdIo_t *p_cdio, unsigned int i_flag)
{
  int ret, sense_avail;
  unsigned char sense_reply[18];

  /* Eject synchronously */
  fprintf(stderr,
    "test_eject_load_cycle: WARNING: EJECTING THE TRAY !\n");
  sleep(2);
  test_load_eject(p_cdio, &sense_avail, sense_reply, 0 | (i_flag & 1));

  fprintf(stderr,
    "test_eject_load_cycle: waiting for 5 seconds. DO NOT TOUCH THE TRAY !\n");
  sleep(3);

  /* Load asynchronously */
  fprintf(stderr,
    "test_eject_load_cycle: WARNING: LOADING THE TRAY !\n");
  sleep(2);
  test_load_eject(p_cdio, &sense_avail, sense_reply, 4 | 2 | (i_flag & 1));

  /* Wait for drive attention */
  ret = test_wait_for_drive(p_cdio, 30, i_flag & 3);
  return ret;
}


/* OBTRUSIVE , PHYSICAL EFFECT , DANGER OF HUMAN INJURY */
/* Eject, wait, test, load. All synchronously.
   @param flag bit0= verbose
   @return    1= all seems well , 0= minor failure , -1= severe failure
*/
static int
test_eject_test_load(CdIo_t *p_cdio, unsigned int i_flag)
{
  int ret, sense_avail;
  unsigned char sense_reply[18];

  /* Eject synchronously */
  fprintf(stderr,
    "test_eject_test_load: WARNING: EJECTING THE TRAY !\n");
  sleep(2);
  test_load_eject(p_cdio, &sense_avail, sense_reply, 0 | (i_flag & 1));

  fprintf(stderr,
    "test_eject_test_load: waiting for 5 seconds. DO NOT TOUCH THE TRAY !\n");
  sleep(3);

  ret = test_test_unit_ready(p_cdio, &sense_avail, sense_reply, i_flag & 1);
  if (ret == 0) {
    fprintf(stderr,
            "test_eject_test_load: Drive ready although tray ejected.\n");
    fprintf(stderr,
            "test_eject_test_load: Test aborted. Tray will stay ejected.\n");
    return -1;
  }
  if (ret == 0 || sense_avail < 18) {
    fprintf(stderr,
 "test_eject_test_load: Only %d sense reply bytes returned. Expected >= 18.\n",
            sense_avail);
    fprintf(stderr,
            "test_eject_test_load: Test aborted. Tray will stay ejected.\n");
    return -1;
  }

  /* Load synchronously */
  fprintf(stderr,
    "test_eject_test_load: WARNING: LOADING THE TRAY !\n");
  sleep(2);
  test_load_eject(p_cdio, &sense_avail, sense_reply, 4 | (i_flag & 1));

  return 1;
}


/* OBTRUSIVE */
/* Read Mode Page 05h "Write Parameters", change a value, write the page,
   check effect, restore old value, check again.
   @param flag bit0= verbose
   @return    1= all seems well , 0= minor failure , -1= severe failure
*/
static int
test_rwr_mode_page(CdIo_t *p_cdio, unsigned int i_flag)
{
  int ret, sense_avail, page_code = 5, subpage_code = 0, alloc_len, i_size;
  int write_type, final_return = 1, new_write_type, old_i_size;
  unsigned char sense_reply[18];
  unsigned char buf[265], old_buf[265];        /* page size is max. 255 + 10 */
  static char w_types[4][8] = {"Packet", "TAO", "SAO", "Raw"};

  alloc_len = 10;
  ret = test_mode_sense(p_cdio, &sense_avail, sense_reply,
                        page_code, subpage_code, alloc_len,
                        buf, &i_size, 2 | (i_flag & 1));
  if (ret != 0) {
    fprintf(stderr, "test_rwr_mode_page: Cannot obtain mode page 05h.\n");
    return 0;
  }
  alloc_len = (i_size <= sizeof(buf)) ? i_size : sizeof(buf);
  ret = test_mode_sense(p_cdio, &sense_avail, sense_reply,
                        page_code, subpage_code, alloc_len,
                        buf, &i_size, (i_flag & 1));
  if (ret != 0) {
    fprintf(stderr, "test_rwr_mode_page: Cannot obtain mode page 05h.\n");
    return 0;
  }
  memcpy(old_buf, buf, sizeof(buf));
  old_i_size = i_size;

  write_type = buf[10] & 0x0f; 
  if (i_flag & 1)
    fprintf(stderr, "test_rwr_mode_page: Write type = %d (%s)\n",
            write_type, write_type < 4 ? w_types[write_type] : "Reserved");

  /* Choose a conservative CD writer setting.
  */
  memset(buf, 0, 8);
  if (write_type == 1) { /* is TAO -> make SAO */ 
    buf[10] = (buf[10] & 0xf0) | 2;   /* SAO */
    new_write_type = 2;
  } else {
    buf[10] = (buf[10] & 0xf0) | 1;   /* TAO */
    new_write_type = 1;
  }
  buf[11] = 4;                        /* Track Mode 4, no multi-session,
                                         variable Packet size */
  buf[12] = 8;                        /* Data Block Type : CD-ROM Mode 1 */
  buf[16] = 0;                        /* Session Format : "CD-DA or CD-ROM" */

  ret = test_mode_select(p_cdio, &sense_avail, sense_reply,
                         buf, i_size, i_flag & 1);
  if (ret != 0) {
    fprintf(stderr, "test_rwr_mode_page: Cannot set mode page 05h.\n");
    if (ret == DRIVER_OP_NOT_PERMITTED) {
      fprintf(stderr,
            "test_rwr_mode_page: DRIVER_OP_NOT_PERMITTED with MODE SELECT.\n");
      return -1;
    }
    return 0;
  }

  /* Read mode page and check whether effect visible in buf[10] */
  ret = test_mode_sense(p_cdio, &sense_avail, sense_reply,
                        page_code, subpage_code, alloc_len,
                        buf, &i_size, (i_flag & 1));
  if (ret != 0) {
    fprintf(stderr, "test_rwr_mode_page: Cannot obtain mode page 05h.\n");
    final_return = final_return > 0 ? 0 : final_return;
  } else if ((buf[10] & 0xf) != new_write_type) {
    fprintf(stderr,
   "test_rwr_mode_page: Mode page did not get into effect. (due %d , is %d)\n",
            new_write_type, buf[10] & 0xf);
    /* One of my DVD burners does this if no media is loaded */
    final_return = final_return > 0 ? 0 : final_return;
  } 

  /* Restore old mode page */
  ret = test_mode_select(p_cdio, &sense_avail, sense_reply,
                         old_buf, old_i_size, i_flag & 1);
  if (ret != 0) {
    fprintf(stderr, "test_rwr_mode_page: Cannot set mode page 05h.\n");
    if (ret == DRIVER_OP_NOT_PERMITTED) {
      fprintf(stderr,
            "test_rwr_mode_page: DRIVER_OP_NOT_PERMITTED with MODE SELECT.\n");
      return -1;
    }
    final_return = final_return > 0 ? 0 : final_return;
  }

  /* Read mode page and check whether old_buf is in effect again */
  ret = test_mode_sense(p_cdio, &sense_avail, sense_reply,
                        page_code, subpage_code, alloc_len,
                        buf, &i_size, (i_flag & 1));
  if (ret != 0) {
    fprintf(stderr, "test_rwr_mode_page: Cannot obtain mode page 05h.\n");
    return final_return > 0 ? 0 : final_return;
  } else if (memcmp(buf, old_buf, i_size) != 0) {
    fprintf(stderr,
            "test_rwr_mode_page: Mode page was not restored to old state.\n");
    final_return = -1;
  } 
  if (i_flag & 1)
    printf("test_rwr_mode_page: Mode page 2Ah restored to previous state\n");
  return final_return;
}


/* ----------- Test of MMC driver enhancements , december 2009 ------------- */


/* OBTRUSIVE */
/* 
   This function bundles tests for the capability to perform MMC commands
   of payload direction SCSI_MMC_DATA_WRITE and to detect the sense reply of
   MMC commands, which indicates error causes or important drive events.

   The default configuration is not very obtrusive to the drive, although the
   drive should not be used for other operations at the same time.
   There are static variables in this function which enable additional
   more obtrusive tests or simulate the lack of write capabilities.
   See the statements about obtrusiveness and undesired side effects above
   the descriptions of the single functions.

   @param drive_path  a drive address suitable for cdio_open_am()
   @param flag        bit0= verbose
   @return            0= no severe failure
                      else an proposal for an exit() value is returned
*/
static int
test_write(char *drive_path, unsigned int i_flag)
{
  int sense_avail = 0, ret, sense_valid, verbose;
  int old_log_level;
  unsigned char sense_reply[18];
  CdIo_t *p_cdio;


  /* If non-0 then the tray will get ejected and loaded again */
  static int with_tray_dance = 0;

  /* If non-0 then an asynchronous test loop will wait 30 s for loaded media */
  static int test_cycle_with_media = 0;

  /* If non-0 then a lack of w-permission will be emulated by using the
     insufficient access mode "IOCTL" */
  static int emul_lack_of_wperm = 0;

  old_log_level = cdio_loglevel_default;
  verbose = i_flag & 1;

  p_cdio = cdio_open_am(drive_path, DRIVER_DEVICE, "MMC_RDWR_EXCL");
  if (!p_cdio) 
    ret = SKIP_TEST - 16;
  else  {
    const char *psz_access_mode = cdio_get_arg(p_cdio, "access-mode");

    if (0 != strncmp(psz_access_mode,
                     "MMC_RDWR_EXCL", strlen("MMC_RDWR_EXCL"))) {
      fprintf(stderr,
              "Got %s; Should get back %s, the access mode requested.\n",
              psz_access_mode, "MMC_RDWR_EXCL");
      {ret = 1; goto ex;}
    }

    /* The further code produces some intentional failures which should not be
       reported by mmc_run_cmd() as INFO messages.
    */
    cdio_loglevel_default = CDIO_LOG_WARN;

  /* Test availability of sense reply in case of unready drive.
     E.g. if the tray is already ejected.
  */
  ret = test_test_unit_ready(p_cdio, &sense_avail, sense_reply, !!verbose);
  if (ret != 0 && sense_avail < 18) {
    fprintf(stderr,
	    "Error: Drive not ready. Only %d sense bytes. Expected >= 18.\n",
	    sense_avail);
    ret = 2; goto ex;
  }

  if (emul_lack_of_wperm) { /* To check behavior with lack of w-permission */
    fprintf(stderr,
	    "test_test: SIMULATING LACK OF WRITE CAPABILITIES by access mode IOCTL\n");
    cdio_destroy(p_cdio);
    p_cdio = cdio_open_am(drive_path, DRIVER_DEVICE, "IOCTL");
  }
    
    
  /* Test write permission */ /* Try whether a mode page 2Ah can be set */
  ret = test_rwr_mode_page(p_cdio, !!verbose);
  if (ret <= 0) {
    if (ret < 0) {
      fprintf(stderr, "Error: test_rwr_mode_page() had severe failure.\n");
      {ret = 3; goto ex;}
    }
    fprintf(stderr, "Warning: test_rwr_mode_page() had minor failure.\n");
  }
  
  
  if (with_tray_dance) {
    /* More surely provoke a non-trivial sense reply */
    if (test_cycle_with_media) {
      /* Eject, wait, load, watch by test unit ready loop */
      ret = test_eject_load_cycle(p_cdio, 2 | !!verbose);
      if (ret <= 0) {
	if (ret < 0) {
	  fprintf(stderr,
		  "Error: test_eject_load_cycle() had severe failure.\n");
	  {ret = 4; goto ex;}
	}
	fprintf(stderr,
		"Warning: test_eject_load_cycle() had minor failure.\n");
      }
    } else {
      /* Eject, test for proper unreadiness, load */
      ret = test_eject_test_load(p_cdio, !!verbose);
      if (ret <= 0) {
	if (ret < 0) {
	  fprintf(stderr,
		  "Error: test_eject_test_load() had severe failure.\n");
	  {ret = 5; goto ex;}
	}
	fprintf(stderr,
		"Warning: test_eject_test_load() had minor failure.\n");
      }
      /* Wait for drive attention */
      test_wait_for_drive(p_cdio, 15, 2 | !!verbose);
    }
  }
  
  /* How are we, finally ? */
  ret = test_test_unit_ready(p_cdio, &sense_valid, sense_reply, !!verbose);
  if ((i_flag & 1) && ret != 0 && sense_reply[2] == 2 && 
      sense_reply[12] == 0x3a)
    fprintf(stderr, "test_unit_ready: Note: No loaded media detected.\n");
  ret = 0;
  }

ex:;
  cdio_loglevel_default = old_log_level;
  cdio_destroy(p_cdio);
  return ret;
}


/* ---------------------------      main       ----------------------------- */


int
main(int argc, const char *argv[])
{
  CdIo_t *p_cdio;
  char **ppsz_drives=NULL;
  const char *psz_source = NULL;
  int ret;
  int exitrc = 0;
  bool b_verbose = (argc > 1);

  cdio_loglevel_default = b_verbose ? CDIO_LOG_DEBUG : CDIO_LOG_INFO;

  ppsz_drives = cdio_get_devices(DRIVER_DEVICE);
  if (!ppsz_drives) {
    printf("Can't find a CD-ROM drive. Skipping test.\n");
    exit(SKIP_TEST);
  }
  
  p_cdio = cdio_open(ppsz_drives[0], DRIVER_DEVICE);
  if (p_cdio) {
    const char *psz_have_mmc = cdio_get_arg(p_cdio, "mmc-supported?");
    
    psz_source = cdio_get_arg(p_cdio, "source");
    if (0 != strncmp(psz_source, ppsz_drives[0],
                     strlen(ppsz_drives[0]))) {
      fprintf(stderr, 
              "Got %s; should get back %s, the name we opened.\n",
              psz_source, ppsz_drives[0]);
      exit(1);
    }

    if ( psz_have_mmc 
	 && 0 == strncmp("true", psz_have_mmc, sizeof("true")) ) {

	/* Test the MMC enhancements of version 0.83 in december 2009 */
	ret = test_write(ppsz_drives[0], 
			cdio_loglevel_default == CDIO_LOG_DEBUG);
	if (ret != 0) exit(ret + 16);
    }

    cdio_destroy(p_cdio);

  } else {
    fprintf(stderr, "cdio_open('%s') failed\n", ppsz_drives[0]);
    exit(2);
  }

  cdio_free_device_list(ppsz_drives);

  return exitrc;
}

