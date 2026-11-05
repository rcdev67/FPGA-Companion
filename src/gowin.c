/*
  gowin.c

*/


#include <string.h>   // for memXXX()

#include "gowin.h"
#include "../debug.h"
#include "../jtag.h"
#include "../spi.h"     // for freetos includes

#ifdef ENABLE_JTAG
#include <ff.h>

// idcode is stored globally so the FPGA routines can address the right FPGA
static uint32_t idcode = 0;

static void gowin_printStatusReg(uint32_t status) {
    if (status & GOWIN_STATUS_CRC_ERROR)            gowin_debugf("Bit 0: CRC ERROR detected");
    if (status & GOWIN_STATUS_BAD_COMMAND)          gowin_debugf("Bit 1: Bad command received");
    if (status & GOWIN_STATUS_ID_VERIFY_FAILED)     gowin_debugf("Bit 2: ID verification failed");
    if (status & GOWIN_STATUS_TIMEOUT)              gowin_debugf("Bit 3: Timeout occurred");
    if (status & GOWIN_STATUS_AUTO_BOOT_2ND_FAIL)   gowin_debugf("Bit 4: Auto boot 2nd failed");
    if (status & GOWIN_STATUS_MEMORY_ERASE)         gowin_debugf("Bit 5: Memory erase in progress");
    if (status & GOWIN_STATUS_PREAMBLE)             gowin_debugf("Bit 6: Preamble detected");
    if (status & GOWIN_STATUS_SYSTEM_EDIT_MODE)     gowin_debugf("Bit 7: System edit mode active");
    if (status & GOWIN_STATUS_PRG_SPIFLASH_DIRECT)  gowin_debugf("Bit 8: Programming SPI flash directly");
    if (status & GOWIN_STATUS_AUTO_BOOT_1ST_FAILED) gowin_debugf("Bit 9: Auto boot 1st failed");
    if (status & GOWIN_STATUS_NON_JTAG_CNF_ACTIVE)  gowin_debugf("Bit 10: Non-JTAG configuration active");
    if (status & GOWIN_STATUS_BYPASS)               gowin_debugf("Bit 11: Bypass mode enabled");
    if (status & GOWIN_STATUS_I2C_SRAM_F)           gowin_debugf("Bit 12: I2C_SRAM_F");
    if (status & GOWIN_STATUS_DONE_FINAL)           gowin_debugf("Bit 13: Done final");
    if (status & GOWIN_STATUS_SECURITY_FINAL)       gowin_debugf("Bit 14: Security final");
    if (status & GOWIN_STATUS_ENCRYPTED_FORMAT)     gowin_debugf("Bit 15: ENCRYPTED_FORMAT");
    if (status & GOWIN_STATUS_KEY_IS_RIGHT)         gowin_debugf("Bit 16: KEY_IS_RIGHT");
    if (status & GOWIN_STATUS_SSPI_MODE)            gowin_debugf("Bit 17: SSPI_MODE");
    if (status & GOWIN_STATUS_SER_CRC_DONE)         gowin_debugf("Bit 18: Serial CRC done");
    if (status & GOWIN_STATUS_SER_CRC_ERR)          gowin_debugf("Bit 19: Serial CRC error");
    if (status & GOWIN_STATUS_SER_ECC_CORR)         gowin_debugf("Bit 20: ECC corrected");
    if (status & GOWIN_STATUS_SER_ECC_UNCORR)       gowin_debugf("Bit 21: ECC uncorrectable");
    if (status & GOWIN_STATUS_SER_RUNNING)          gowin_debugf("Bit 22: Serial running");
    if (status & GOWIN_STATUS_CPU_BUS_WIDTH_0)      gowin_debugf("Bit 23: CPU_BUS_WIDTH_0");
    if (status & GOWIN_STATUS_CPU_BUS_WIDTH_1)      gowin_debugf("Bit 24: CPU_BUS_WIDTH_1");
#ifndef TANG_MEGA138KPRO
    if (status & GOWIN_STATUS_SYNC_DET_TERY_0)      gowin_debugf("Bit 25: SYNC_DET_TERY_0");
    if (status & GOWIN_STATUS_SYNC_DET_TERY_1)      gowin_debugf("Bit 26: SYNC_DET_TERY_1");
    if (status & GOWIN_STATUS_DECOMP_FAIL)          gowin_debugf("Bit 27: Decompression failed");
    if (status & GOWIN_STATUS_MFG_DONE)             gowin_debugf("Bit 28: Manufacturing done");
    if (status & GOWIN_STATUS_INIT)                 gowin_debugf("Bit 29: Initialization complete");
    if (status & GOWIN_STATUS_WAKEUP)               gowin_debugf("Bit 30: Wakeup signal");
    if (status & GOWIN_STATUS_AUTO_ERASE)           gowin_debugf("Bit 31: Auto erase enabled");
#endif
}

static uint32_t gowin_readStatusReg(void) {
  uint32_t status = jtag_command_u08_read32(GOWIN_COMMAND_STATUS);
  // gowin_printStatusReg(status);
  return status;
}

static void gowin_gw2a_force_state(void) {
  /* undocumented sequence but required when
   * flash failure
   */
  uint32_t state =  gowin_readStatusReg();
  if ((state & GOWIN_STATUS_CRC_ERROR) == 0)
    return;
  jtag_command_u08(GOWIN_COMMAND_CONFIG_DISABLE);
  jtag_command_u08(0);
  jtag_command_u08_read32(GOWIN_COMMAND_IDCODE);
  state = gowin_readStatusReg();
  jtag_command_u08(GOWIN_COMMAND_CONFIG_DISABLE);
  jtag_command_u08(0);
  state = gowin_readStatusReg();
  jtag_command_u08_read32(GOWIN_COMMAND_IDCODE);
  jtag_command_u08(GOWIN_COMMAND_CONFIG_ENABLE);
  jtag_command_u08(GOWIN_COMMAND_CONFIG_DISABLE);
  jtag_command_u08(GOWIN_COMMAND_NOOP);
  jtag_command_u08_read32(GOWIN_COMMAND_IDCODE);
  jtag_command_u08(GOWIN_COMMAND_NOOP);
  jtag_command_u08_read32(GOWIN_COMMAND_IDCODE);
}

static bool gowin_pollFlag(uint32_t mask, uint32_t value) {
  uint32_t status;
  int timeout = 0;
  do {
    status = gowin_readStatusReg();
    if (timeout == 1000){  // TODO: was 100000000
      gowin_debugf("pollFlag(): timeout");
      gowin_printStatusReg(status);
      return false;
    }
    timeout++;
  } while ((status & mask) != value);
  
  return true;
}

static bool gowin_enableCfg(void) {
  jtag_command_u08(GOWIN_COMMAND_CONFIG_ENABLE);
  return gowin_pollFlag(GOWIN_STATUS_SYSTEM_EDIT_MODE, GOWIN_STATUS_SYSTEM_EDIT_MODE);
}

static bool gowin_disableCfg(void) {
  jtag_command_u08(GOWIN_COMMAND_CONFIG_DISABLE);
  jtag_command_u08(GOWIN_COMMAND_NOOP);
  return gowin_pollFlag(GOWIN_STATUS_SYSTEM_EDIT_MODE, 0);
}

// prepare SRAM upload
static bool gowin_eraseSRAM(void) {
  uint32_t status;
  jtag_command_u08_read32(GOWIN_COMMAND_USERCODE);

  // Clearing if failed loading
  status = gowin_readStatusReg();

  if ((idcode == IDCODE_GW5AST138)||(idcode == IDCODE_GW5A25)) {
    if ((status & GOWIN_STATUS_DONE_FINAL) == 0) {
      gowin_debugf("FPGA REINIT");
      jtag_command_u08(GOWIN_COMMAND_REINIT);
      jtag_clk_us(10000);
    }
  }
  // mandatory for GW5A to avoid SRAM clear errors
  if ((idcode == IDCODE_GW5AT60) || (idcode == IDCODE_GW5AST138) || (idcode == IDCODE_GW5A25)) {
      gowin_debugf("FPGA REINIT");
      jtag_command_u08(GOWIN_COMMAND_REINIT);
      jtag_clk_us(10000);
    }

  // Clearing Status Code Errors
  if (idcode != IDCODE_GW2AR18) {
    status = gowin_readStatusReg();
    bool auto_boot_2nd_fail = (status & GOWIN_STATUS_AUTO_BOOT_2ND_FAIL) == GOWIN_STATUS_AUTO_BOOT_2ND_FAIL;
    bool is_timeout = (status & GOWIN_STATUS_TIMEOUT) == GOWIN_STATUS_TIMEOUT;
    bool bad_cmd = (status & GOWIN_STATUS_BAD_COMMAND) == GOWIN_STATUS_BAD_COMMAND;
    bool id_verify_failed = (status & GOWIN_STATUS_ID_VERIFY_FAILED) ==  GOWIN_STATUS_ID_VERIFY_FAILED;
    if (is_timeout || auto_boot_2nd_fail || bad_cmd || id_verify_failed) {
    gowin_debugf("Clearing status errors by FPGA RELOAD");
    gowin_printStatusReg(status);
    jtag_command_u08(GOWIN_COMMAND_NOOP);
    jtag_command_u08(GOWIN_COMMAND_CONFIG_ENABLE);
    jtag_command_u08(GOWIN_COMMAND_RECONFIG);
    jtag_command_u08(GOWIN_COMMAND_NOOP);
    jtag_clk_us(100000);
    jtag_command_u08(GOWIN_COMMAND_CONFIG_DISABLE);
    jtag_command_u08(GOWIN_COMMAND_NOOP);
    jtag_clk_us(100000);
    }
  }

  if(idcode == IDCODE_GW2AR18) {
    gowin_gw2a_force_state();

    if(!gowin_enableCfg()) {
      status = gowin_readStatusReg();
      gowin_printStatusReg(status);
      gowin_debugf("Failed to enable config");
      return false;
     }
  } else {
      jtag_command_u08(GOWIN_COMMAND_CONFIG_ENABLE);
      // no status polling !
   }

  jtag_command_u08(GOWIN_COMMAND_ERASE_SRAM);
  jtag_command_u08(GOWIN_COMMAND_NOOP);
  jtag_clk_us(10000); // wait for erase to complete
#ifdef TANG_MEGA138KPRO
  jtag_clk_us(40000);
#endif

  if(!gowin_pollFlag(GOWIN_STATUS_MEMORY_ERASE, GOWIN_STATUS_MEMORY_ERASE)) {
    status = gowin_readStatusReg();
    gowin_printStatusReg(status);
    gowin_debugf("Failed to trigger SRAM erase");
    return false;
  }

  jtag_command_u08(GOWIN_COMMAND_XFER_DONE);
  jtag_command_u08(GOWIN_COMMAND_NOOP);
  if(!gowin_disableCfg()) {
    status = gowin_readStatusReg();
    gowin_printStatusReg(status);
    gowin_debugf("Failed to disable config");
    return false;
  }

  return true;
}

static void gowin_writeSRAM_prepare(void) {
  jtag_command_u08(GOWIN_COMMAND_CONFIG_ENABLE); // config enable

  /* UG704 3.4.3 */
  jtag_command_u08(GOWIN_COMMAND_INIT_ADDR); // address initialize

  /* 2.2.6.4 */
  jtag_command_u08(GOWIN_COMMAND_XFER_WRITE); // transfer configuration data

  /* finally enter shift DR state */
  jtag_enter_shiftDR();
}

#ifndef MCU_HW_JTAG_GPIO_OUT_MODE
static void gowin_writeSRAM_transfer(uint8_t *data, uint16_t len, bool last) {
  jtag_shiftDR_part(data, NULL, 8*len, last);
}
#endif

static bool gowin_writeSRAM_postproc(__attribute__((unused)) uint32_t checksum) {
  jtag_command_u08(GOWIN_COMMAND_CONFIG_DISABLE);
  jtag_command_u08(GOWIN_COMMAND_NOOP);

  uint32_t status_reg = gowin_readStatusReg();
  status_reg = gowin_readStatusReg(); // read twice to get updated status
  if(!(status_reg & GOWIN_STATUS_DONE_FINAL)) {
    gowin_printStatusReg(status_reg);
    fatal_debugf("Failed to write SRAM");
    return false;
  }
  uint32_t usercode = jtag_command_u08_read32(GOWIN_COMMAND_USERCODE);
  gowin_debugf("SRAM successfully written, Usercode=0x%08lx", usercode);
  return true;
}

void gowin_fpgaReset(void) {
    gowin_debugf("FPGA RECONFIG");

    jtag_command_u08(GOWIN_COMMAND_RECONFIG);
    jtag_command_u08(GOWIN_COMMAND_NOOP);
    jtag_clk_us(100000);
}

static inline uint8_t reverse_byte(uint8_t byte) {
  byte = ((byte & 0x55) << 1) | ((byte & 0xaa) >> 1);
  byte = ((byte & 0x33) << 2) | ((byte & 0xcc) >> 2);
  return ((byte & 0x0f) << 4) | ((byte & 0xf0) >> 4);
}

static bool gowin_open(void) {
  idcode = jtag_open();
  gowin_debugf("FPGA detected: %08lx", idcode);

  return(idcode == IDCODE_GW2AR18  || 
         idcode == IDCODE_GW5AT60  || 
         idcode == IDCODE_GW5A25  ||
         idcode == IDCODE_GW5AST138);
}

// try to open "core.bin" on the sd card and try to transfer it via JTAG to
// the FPGA
static bool gowin_upload_core_bin(const char *name) {
  // get file size
  FILINFO fno;
  if (f_stat(name, &fno) != FR_OK) {
    gowin_debugf("Unable to determine length of %s", name);
    return false;
  }

  // try to open core.bin
  FIL fil;
  if(f_open(&fil, name, FA_OPEN_EXISTING | FA_READ) != FR_OK) {
    gowin_debugf("Opening %s failed", name);
    return false;
  }
    
  if(!gowin_open()) {
    gowin_debugf("FPGA not detected");
    jtag_close();
    f_close(&fil);
    return false;
  }
  
  // measure total download time
  TickType_t ticks = xTaskGetTickCount();
  
  gowin_debugf("=== Erase SRAM ===");
  if(!gowin_eraseSRAM()) {
    gowin_debugf("Failed to erase SRAM");
    jtag_close();
    f_close(&fil);
    return false;
  }

  if (idcode == IDCODE_GW5AST138) {
    gowin_debugf("=== 2nd Erase SRAM ===");
    if(!gowin_eraseSRAM()) {
      gowin_debugf("Failed to erase SRAM");
      jtag_close();
      f_close(&fil);
      return false;
    }
  }
 
  gowin_debugf("=== Load SRAM ===");
  gowin_writeSRAM_prepare();
    
  FRESULT fr;
  uint8_t buffer[512];
  UINT bytesRead;
  uint32_t total = 0;

#ifdef MCU_HW_JTAG_GPIO_OUT_MODE
  mcu_hw_jtag_enter_gpio_out_mode();
#endif
  
  do {
    if((fr = f_read(&fil, buffer, sizeof(buffer), &bytesRead)) == FR_OK) {
      total += bytesRead;

#ifndef MCU_HW_JTAG_GPIO_OUT_MODE
      for(UINT i=0;i<bytesRead;i++) buffer[i] = reverse_byte(buffer[i]);
      gowin_writeSRAM_transfer(buffer, bytesRead, total >= fno.fsize);
#else
      mcu_hw_jtag_writeTDI_msb_first_gpio_out_mode(buffer, bytesRead, total >= fno.fsize);
#endif
    }
  } while(fr == FR_OK && total < fno.fsize);

#ifdef MCU_HW_JTAG_GPIO_OUT_MODE
  mcu_hw_jtag_exit_gpio_out_mode();

  // send TMS 1/0 to return into RUN-TEST/IDLE
  mcu_hw_jtag_tms(1, 0b01, 2);    // normally this happens in gowin_writeSRAM_transfer/jtag_shiftDR_part
#endif  

  if(fr != FR_OK) {
    fatal_debugf("Binary download failed after %lu bytes", total);
    jtag_close();
    f_close(&fil);
    return false;	
  }

  // don't set checksum
  gowin_writeSRAM_postproc(0xffffffff);
  
  gowin_debugf("Read %lu bytes", total);
  
  ticks = xTaskGetTickCount() - ticks;
  gowin_debugf("Download time: %lu.%03lu seconds", ticks/1000, ticks%1000);
  
  jtag_close();
  f_close(&fil);

  return true;
}

// try to open on the sd card and try to transfer it via JTAG into the FPGA
static bool gowin_upload_core_fs(const char *name) {  
  // try to open core.fs
  FIL fil;
  if(f_open(&fil, name, FA_OPEN_EXISTING | FA_READ) != FR_OK) {
    gowin_debugf("%s not found", name);
    return false;
  }
    
  // transmit core via JTAG
  if(!gowin_open()) {
    gowin_debugf("FPGA not detected");    
    jtag_close();
    f_close(&fil);
    return false;
  }
  
  // measure total download time
  TickType_t ticks = xTaskGetTickCount();
  
  gowin_debugf("=== Erase SRAM ===");
  if(!gowin_eraseSRAM()) {
    gowin_debugf("Failed to erase SRAM");
    jtag_close();
    f_close(&fil);
    return false;
  }
  
  gowin_debugf("=== Load SRAM ===");
  gowin_writeSRAM_prepare();
  
  // =================== parse the fs file =================
  // these are rather huge (>7MB for the GW2AR-18) and mainly contain
  // ASCII encoded binary data with some text header
  uint8_t buffer[128];
  UINT bytesRead;
  FRESULT fr;
  uint32_t total = 0;
  bool header_done = false;
  bool parse_error = false;
  uint16_t used = 0;
  
  // --------- parse the .fs header -----------
  do {
    if((fr = f_read(&fil, buffer+used, sizeof(buffer)-used, &bytesRead)) == FR_OK) {
      total += bytesRead;
      
      // check if the line starts with a binary digit
      if(buffer[0] == '0' || buffer[0] == '1') {
	gowin_debugf("header done");
	used += bytesRead;
	header_done = true;
      } else {
	// the buffer should start with "//" and there should be a newline in this buffer
	if(buffer[0] != '/' || buffer[1] != '/')
	  parse_error = true;
	else {	    
	  uint8_t *nl = memchr(buffer, '\n', sizeof(buffer));
	  if(!nl) parse_error = true;  // that would not happen with a valid fs file
	  else {
	    // there's a complete header line in the buffer. Terminate it
	    *nl = '\0';
	    // line may have DOS line endings. In that case terminate earlier
	    if(*(nl-1) == '\r') *(nl-1) = '\0';
	    
	    gowin_debugf("header: %s", buffer);
	    
	    // shift unused part of buffer down
	    memmove(buffer, nl+1, sizeof(buffer)-(nl-buffer+1));
	    used = sizeof(buffer)-(nl-buffer+1);
	  }
	}	    
      }
    }
  } while(fr == FR_OK && bytesRead && !header_done && !parse_error);
  
  // TODO: check header values to e.g. make sure the bitstream is for the
  // correct FPGA type
    
  if(fr != FR_OK || !header_done || parse_error) {
    gowin_debugf("Header parsing failed");
    jtag_close();
    f_close(&fil);
    return false;
  }
  
  // --------- parse the .fs binary data -----------
  
  gowin_debugf("Parsing and uploading binary bitstream");
    
  uint8_t data[512];  // one line holds up to 430 bytes
  uint8_t byte = 0, bit = 0;
  uint32_t total_data = 0, line_data = 0;
  uint16_t line = 0;
  
#ifdef MCU_HW_JTAG_GPIO_OUT_MODE
  mcu_hw_jtag_enter_gpio_out_mode();
#endif
  
  do {
    // collect all 0/1 bits
    for(int i=0;i<used && !parse_error;i++) {
      if(buffer[i] == '0' || buffer[i] == '1') {
#ifdef MCU_HW_JTAG_GPIO_OUT_MODE
	// in gpio out mode the bit order is different
	byte = (byte << 1)|((buffer[i] == '1')?1:0);
#else
	byte = (byte >> 1)|((buffer[i] == '1')?0x80:0);
#endif
	
	bit = (bit+1)&7;
	if(!bit) {
	  // got a complete byte
	  data[line_data++] = byte;
	  total_data++;
	  byte = 0;
	}
      } else {
	// the digits should always come in multiple of 8
	if(bit) {
	  fatal_debugf("incomplete byte");
	  parse_error = true;
	} else if(buffer[i] == '\n') {
	  
	  // gowin_debugf("line %d: %ld (total %ld, used %d)", line, line_data, total_data, used);
	  // hexdump(data, line_data);
#ifndef MCU_HW_JTAG_GPIO_OUT_MODE  
	  gowin_writeSRAM_transfer(data, line_data, line>100 && line_data == 2);
#else
	  mcu_hw_jtag_writeTDI_msb_first_gpio_out_mode(data, line_data, line>100 && line_data == 2);
#endif
	  
	  line_data = 0;
	  line++;
	}
      }
    }
    
    if(!parse_error) {
      // refill the buffer
      if((fr = f_read(&fil, buffer, sizeof(buffer), &bytesRead)) == FR_OK) {
	total += bytesRead;
	used = bytesRead;
      }
    }
  } while(fr == FR_OK && bytesRead && !parse_error);
  
#ifdef MCU_HW_JTAG_GPIO_OUT_MODE
  mcu_hw_jtag_exit_gpio_out_mode();

  // send TMS 1/0 to return into RUN-TEST/IDLE
  mcu_hw_jtag_tms(1, 0b01, 2);    // normally this happens in gowin_writeSRAM_transfer/jtag_shiftDR_part
#endif
  
  if((fr != FR_OK) || parse_error) {
    fatal_debugf("Binary download failed after %lu bytes", total);
    jtag_close();
    f_close(&fil);
    return false;	
  }
  
  gowin_debugf("Total data transferred: %ld bytes", total_data);

  // TODO: Figure out where the checksum is supposed to come from. The checksum openFPGAloader
  // downloads is not the one mentioned in the .fs header.
  gowin_writeSRAM_postproc(0xffffffff);
  
  gowin_debugf("Read %lu bytes", total);
  
  ticks = xTaskGetTickCount() - ticks;
  gowin_debugf("Download time: %lu.%03lu seconds", ticks/1000, ticks%1000);
  
  jtag_close();
  f_close(&fil);
  return true;
}

bool gowin_upload_core(const char *name) {
  if(strcasecmp(name + strlen(name) - 4, ".bin") == 0)
    return gowin_upload_core_bin(name);

  if(strcasecmp(name + strlen(name) - 3, ".fs") == 0)
    return gowin_upload_core_fs(name);
    
  gowin_debugf("Unknown file type");
  return false;
}

#include <stdio.h>

/* ---- the FPGA's SPI flash through JTAG, GW2A only ----
   The feasibility test for writing a core from the USB stick into the
   flash, so that it stays. On the GW2A(R) the JTAG instruction 0x16
   passes what is shifted through the data register straight to the SPI
   flash: TDI is MOSI, TDO MISO, and the flash is selected while in
   SHIFT-DR. JTAG sends LSB first, SPI MSB first, so every byte is
   reversed, and what comes back is one bit late. This follows
   openFPGALoader (src/gowin.cpp, spi_put() for is_gw2a, Apache-2.0,
   Gwenhael Goavec-Merou and contributors). The SRAM has to be erased
   first: the running core lets go of the flash only then. */

// Bits shifted after a command that only sends (rx NULL). Reading needs a
// byte more for the answer, which comes a bit late; whether sending loses
// a bit the same way is found out at the flash itself, see
// gowin_flash_core(): its write enable latch shows whether a command got in.
static uint8_t spi_tx_extra = 0;

// One SPI transaction: tx out, the same number of bytes in, rx may be NULL
static void gowin_spi_xfer(const uint8_t *tx, uint8_t *rx, uint16_t len) {
  uint16_t n = len + (rx ? 1 : 0);       // one byte more to catch the late bit
  uint8_t jtx[n + 1], jrx[n + 1];
  memset(jtx, 0, sizeof(jtx));
  memset(jrx, 0, sizeof(jrx));
  for(uint16_t i = 0; i < len; i++) jtx[i] = reverse_byte(tx[i]);

  jtag_command_u08(GOWIN_COMMAND_TRANSFER_SPI);

  // RUN-TEST/IDLE -> SELECT-DR -> CAPTURE-DR -> EXIT1-DR -> PAUSE-DR -> EXIT2-DR,
  // as openFPGALoader's set_state(EXIT2_DR), then EXIT2-DR -> SHIFT-DR
  mcu_hw_jtag_tms(0, 0b10101, 5);
  mcu_hw_jtag_tms(0, 0b0, 1);

  // all but the last bit, the last one with the step out of SHIFT-DR
  uint32_t bits = 8 * n + (rx ? 0 : spi_tx_extra);
  uint32_t last = bits - 1;
  mcu_hw_jtag_data(jtx, jrx, last);
  uint8_t lrx = mcu_hw_jtag_tms((jtx[last / 8] >> (last % 8)) & 1, 0b1, 1);
  if(rx) jrx[n - 1] = (jrx[n - 1] & 0x7f) | (lrx & 0x80);
  mcu_hw_jtag_tms(1, 0b01, 2);           // EXIT1-DR -> UPDATE-DR -> RUN-TEST/IDLE

  if(rx)
    for(uint16_t i = 0; i < len; i++)
      rx[i] = reverse_byte(jrx[i] >> 1) | (jrx[i + 1] & 0x01);
}

// Read the flash's JEDEC id and the first bytes of two places that hold
// something known: 0x100000, the ST's first TOS slot (a TOS starts with a
// BRA.S, 0x60), and 0x20C000, the 1541's CBM DOS (97 AA AA AA). A board
// has at least one of them if a machine ever ran on it. Nothing is
// written. The FPGA is empty afterwards; the caller loads a core into its
// SRAM. out gets one line.
static void gowin_flash_read(uint32_t addr, uint8_t *d, uint16_t len) {
  uint8_t tx[4 + len], rx[4 + len];
  memset(tx, 0, sizeof(tx));
  tx[0] = 0x03;
  tx[1] = addr >> 16; tx[2] = addr >> 8; tx[3] = addr;
  gowin_spi_xfer(tx, rx, sizeof(tx));
  memcpy(d, rx + 4, len);
}

bool gowin_flash_probe(char *out, size_t size) {
  if(!gowin_open()) {
    snprintf(out, size, "No FPGA on JTAG (%08lx)", (unsigned long)idcode);
    jtag_close();
    return false;
  }
  if(idcode != IDCODE_GW2AR18) {
    snprintf(out, size, "Not a GW2AR-18 (%08lx)", (unsigned long)idcode);
    jtag_close();
    return false;
  }
  if(!gowin_eraseSRAM()) {
    snprintf(out, size, "Erasing the SRAM failed");
    gowin_fpgaReset();
    jtag_close();
    return false;
  }

  uint8_t idtx[4] = { 0x9f, 0, 0, 0 }, idrx[4];
  gowin_spi_xfer(idtx, idrx, sizeof(idtx));
  uint8_t tos[8], cbm[8];
  gowin_flash_read(0x100000, tos, sizeof(tos));
  gowin_flash_read(0x20C000, cbm, sizeof(cbm));

  gowin_debugf("FLASH: JEDEC %02x %02x %02x", idrx[1], idrx[2], idrx[3]);
  gowin_debugf("FLASH: 0x100000 %02x %02x %02x %02x %02x %02x %02x %02x",
               tos[0], tos[1], tos[2], tos[3], tos[4], tos[5], tos[6], tos[7]);
  gowin_debugf("FLASH: 0x20C000 %02x %02x %02x %02x %02x %02x %02x %02x",
               cbm[0], cbm[1], cbm[2], cbm[3], cbm[4], cbm[5], cbm[6], cbm[7]);

  bool id_ok = !((idrx[1] == 0x00 && idrx[2] == 0x00) || (idrx[1] == 0xff && idrx[2] == 0xff));
  bool tos_ok = (tos[0] == 0x60);
  bool cbm_ok = (cbm[0] == 0x97) && (cbm[1] == 0xaa) && (cbm[2] == 0xaa) && (cbm[3] == 0xaa);
  bool ok = id_ok && (tos_ok || cbm_ok);
  snprintf(out, size, "%02x%02x%02x T:%02x%02x C:%02x%02x %s",
           idrx[1], idrx[2], idrx[3], tos[0], tos[1], cbm[0], cbm[1], ok ? "OK" : "??");

  // Out of SPI mode. The FPGA stays empty: on the Tang Nano 20K a reload
  // from the flash through JTAG does not bring the core back -- Gowin's own
  // programmer cannot either (Reprogram leaves status 0x0020) -- so the
  // caller loads a core into the SRAM, which works.
  mcu_hw_jtag_tms(1, 0b11111, 5);
  jtag_close();
  return ok;
}

/* ---- a core into the flash, so that it stays: "Change core" ----
   The bytes are the ones the SRAM gets: a .bin as it is, a .fs parsed
   with the highest bit first, its "//" lines skipped -- as openFPGALoader
   writes a flash. 64 KB blocks erased as the writing reaches them, 256
   byte pages written and every page read back and compared. Nothing at
   or above CORE_FLASH_LIMIT is touched: the ST's TOS and the 1541's ROMs
   live there. The SRAM is erased first and stays empty; the caller loads
   the core into it afterwards (a reload from the flash through JTAG does
   not work on the Tang Nano 20K). */
#define CORE_FLASH_LIMIT 0x100000

typedef struct {
  FIL fil;
  bool fs;                 // ASCII .fs, else a binary .bin
  bool line_start, comment;
  uint8_t in[256];
  UINT in_len, in_pos;
  uint8_t byte, bit;
} core_src_t;

static int core_getc(core_src_t *s) {
  if(s->in_pos >= s->in_len) {
    if(f_read(&s->fil, s->in, sizeof(s->in), &s->in_len) != FR_OK || !s->in_len) return -1;
    s->in_pos = 0;
  }
  return s->in[s->in_pos++];
}

// up to max bytes of the bitstream, 0 at its end, -1 on an error
static int core_read(core_src_t *s, uint8_t *buf, int max) {
  int n = 0;
  while(n < max) {
    int c = core_getc(s);
    if(c < 0) break;
    if(!s->fs) { buf[n++] = c; continue; }
    if(c == '\n') { s->line_start = true; s->comment = false; continue; }
    if(s->line_start && c == '/') s->comment = true;
    s->line_start = false;
    if(s->comment || (c != '0' && c != '1')) continue;
    s->byte = (s->byte << 1) | (c == '1');
    if(++s->bit == 8) { buf[n++] = s->byte; s->bit = 0; s->byte = 0; }
  }
  if(n == 0 && s->fs && s->bit) return -1;    // the digits did not end on a byte
  return n;
}

// A .fs says in its header which FPGA it is for: it has to be ours
static bool core_header_ok(const char *name) {
  FIL f;
  char line[96];
  bool ok = false;
  if(f_open(&f, name, FA_OPEN_EXISTING | FA_READ) != FR_OK) return false;
  for(int i = 0; i < 20 && f_gets(line, sizeof(line), &f); i++) {
    if(line[0] != '/') break;
    if(strstr(line, "GW2AR-18") || strstr(line, "GW2AR-LV18")) { ok = true; break; }
  }
  f_close(&f);
  return ok;
}

static uint8_t gowin_flash_status(void) {
  uint8_t tx[2] = { 0x05, 0 }, rx[2];
  gowin_spi_xfer(tx, rx, sizeof(tx));
  return rx[1];
}

static bool gowin_flash_wait(uint32_t ms) {
  TickType_t t0 = xTaskGetTickCount();
  while(gowin_flash_status() & 0x01) {
    if((xTaskGetTickCount() - t0) > pdMS_TO_TICKS(ms)) return false;
    vTaskDelay(1);
  }
  return true;
}

static void gowin_flash_wren(void) {
  uint8_t tx[1] = { 0x06 };
  gowin_spi_xfer(tx, NULL, sizeof(tx));
}

static bool gowin_flash_erase64(uint32_t addr) {
  uint8_t tx[4] = { 0xd8, addr >> 16, addr >> 8, addr };
  gowin_flash_wren();
  gowin_spi_xfer(tx, NULL, sizeof(tx));
  return gowin_flash_wait(3000);
}

static bool gowin_flash_page(uint32_t addr, const uint8_t *d, uint16_t len) {
  uint8_t tx[4 + 256];
  tx[0] = 0x02; tx[1] = addr >> 16; tx[2] = addr >> 8; tx[3] = addr;
  memcpy(tx + 4, d, len);
  gowin_flash_wren();
  gowin_spi_xfer(tx, NULL, 4 + len);
  return gowin_flash_wait(100);
}

// Whether the ROMs a machine needs are in the flash: 1 = yes, 0 = no
static void gowin_flash_roms(bool *tos, bool *cbm) {
  uint8_t d[4];
  gowin_flash_read(0x100000, d, 2);
  *tos = (d[0] == 0x60);
  gowin_flash_read(0x20C000, d, 4);
  *cbm = (d[0] == 0x97) && (d[1] == 0xaa) && (d[2] == 0xaa) && (d[3] == 0xaa);
}

// Write the core in name into the flash. NULL if it went through, else a
// short reason. *written gets the bytes written, *tos and *cbm whether the
// ST's TOS and the 1541's DOS are in the flash.
static char flash_diag[32];
const char *gowin_flash_diag(void) { return flash_diag; }

const char *gowin_flash_core(const char *name, uint32_t *written, bool *tos, bool *cbm) {
  static char why[64];
  flash_diag[0] = 0;
  *written = 0; *tos = *cbm = false;
  size_t nl = strlen(name);
  bool fs = (nl > 3) && !strcasecmp(name + nl - 3, ".fs");
  if(fs && !core_header_ok(name)) return "not a core for this FPGA";

  core_src_t *s = pvPortMalloc(sizeof(core_src_t));
  if(!s) return "out of memory";
  memset(s, 0, sizeof(core_src_t));
  s->fs = fs;
  s->line_start = true;
  if(f_open(&s->fil, name, FA_OPEN_EXISTING | FA_READ) != FR_OK) { vPortFree(s); return "cannot open the file"; }

  const char *err = NULL;
  if(!gowin_open() || idcode != IDCODE_GW2AR18) { err = "no GW2AR-18 on JTAG"; goto out; }
  if(!gowin_eraseSRAM()) { err = "erasing the SRAM failed"; goto out; }

  gowin_flash_roms(tos, cbm);

  // Does a command that only sends get into the flash? Its write enable
  // latch (status bit 1) tells: set after WREN. If not with the exact
  // number of bits, then with one more -- reading needs a late bit too.
  uint8_t sr0 = gowin_flash_status(), sr1, sr2 = 0;
  spi_tx_extra = 0;
  gowin_flash_wren();
  sr1 = gowin_flash_status();
  if(!(sr1 & 0x02)) {
    spi_tx_extra = 1;
    gowin_flash_wren();
    sr2 = gowin_flash_status();
    if(!(sr2 & 0x02)) {
      spi_tx_extra = 0;
      snprintf(why, sizeof(why), "write enable does not take (%02x %02x %02x)", sr0, sr1, sr2);
      err = why;
      goto out;
    }
  }
  snprintf(flash_diag, sizeof(flash_diag), "sr %02x, %s", sr0, spi_tx_extra ? "+1 bit" : "exact");
  gowin_debugf("FLASH: %s", flash_diag);

  // Block protection bits set? Gowin's programmer clears them before it
  // writes, and so do we; the region written is limited anyway.
  if(sr0 & 0x1c) {
    uint8_t tx[2] = { 0x01, sr0 & ~0x9c };   // BP0-2 and SRP0 off
    gowin_flash_wren();
    gowin_spi_xfer(tx, NULL, sizeof(tx));
    gowin_flash_wait(100);
    uint8_t sr = gowin_flash_status();
    if(sr & 0x1c) {
      snprintf(why, sizeof(why), "the flash stays write protected (%02x)", sr);
      err = why;
      goto out;
    }
  }

  uint8_t page[256], back[256];
  uint32_t addr = 0;
  TickType_t t0 = xTaskGetTickCount();
  for(;;) {
    int n = core_read(s, page, sizeof(page));
    if(n < 0) { err = "the file is damaged"; break; }
    if(n == 0) break;
    if(addr + n > CORE_FLASH_LIMIT) { err = "the core is too big"; break; }
    if(!(addr & 0xffff) && !gowin_flash_erase64(addr)) {
      snprintf(why, sizeof(why), "erasing at %06lx failed", (unsigned long)addr); err = why; break;
    }
    if(!gowin_flash_page(addr, page, n)) {
      snprintf(why, sizeof(why), "writing at %06lx failed", (unsigned long)addr); err = why; break;
    }
    gowin_flash_read(addr, back, n);
    if(memcmp(page, back, n)) {
      snprintf(why, sizeof(why), "wrong data at %06lx", (unsigned long)addr); err = why; break;
    }
    addr += n;
  }
  *written = addr;
  gowin_debugf("FLASH: %lu bytes of %s in %lu ms%s%s", (unsigned long)addr, name,
               (unsigned long)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS),
               err ? ", " : "", err ? err : "");

out:
  mcu_hw_jtag_tms(1, 0b11111, 5);    // out of SPI mode
  jtag_close();
  f_close(&s->fil);
  vPortFree(s);
  return err;
}

#endif // ENABLE_JTAG
