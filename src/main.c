/*
  main.c - MiSTeryNano FPGA Companion Pi Pico variant

*/

#include "../mcu_hw.h"

#include "../config.h"
#include "../sysctrl.h"
#include "../sdc.h"
#include "../osd.h"
#include "../menu.h"
#include "../inifile.h"
#include "../debug.h"
#include "../ftpd.h"
#include "../telnetd.h"
#include "../xml.h"
#include "../at_wifi.h"
#include "../net.h"

/*-----------------------------------------------------------*/
/*---            main FPGA communication task            ----*/
/*-----------------------------------------------------------*/

TaskHandle_t com_task_handle = NULL;

// Run the ready action. This will usually get the core out of reset.
// On setups not using core configs, just release FPGA from reset
// But this should actually never be the case nowadays.
static void core_ready(void) {
  if(!cfg) sys_set_val('R', 0);
  else     sys_run_action_by_name("ready");
}

#ifdef ENABLE_JTAG
#include "gowin.h"

// The flash test, asked for from the system menu with FLASHTST.REQ on the
// card. It stops the core, so it runs here, before anything else talks to
// the FPGA; the core comes back from the flash and start-up goes on. The
// result goes to FLASHTST.TXT on the card and into the OSD.
static char flash_test_result[64];

static void flash_test_at_boot(void) {
  FILINFO fno;
  if(f_stat(CARD_MOUNTPOINT "/FLASHTST.REQ", &fno) != FR_OK) return;
  f_unlink(CARD_MOUNTPOINT "/FLASHTST.REQ");   // once, whatever happens

  debugf("Flash test requested");
  bool ok = gowin_flash_probe(flash_test_result, sizeof(flash_test_result));
  debugf("Flash test: %s (%s)", flash_test_result, ok ? "ok" : "failed");

  // the core is configured from the flash again
  if(!sys_wait4fpga())
    debugf("Flash test: the core did not come back");

  FIL f;
  if(f_open(&f, CARD_MOUNTPOINT "/FLASHTST.TXT", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
    f_puts(flash_test_result, &f);
    f_puts("\r\n", &f);
    f_close(&f);
  }
}
#endif

static void com_task(__attribute__((unused)) void *p ) {
  debugf("Starting main communication task");
  
  // startup FPGA, this will also put the core into reset
  if(sys_wait4fpga()) {
    // FPGA is ready and can be talked to

    // initialitze SD card
    sdc_init();

#ifdef ENABLE_JTAG
    flash_test_at_boot();
#endif

    // try to load the global config
    inifile_config_read();
    
    // try to load a config .xml from sd card. If the core has identified itself,
    // then e.g. atarist.xml will be read. otherwise config.xml
    FIL fil;
    if(f_open(&fil, sys_get_config_name(), FA_OPEN_EXISTING | FA_READ) == FR_OK) {
      config_init();

      UINT br; char c;
      debugf("Loading XML config from file");

      // read byte by byte. Slow but that doesn't hurt ...
      FRESULT r = f_read(&fil, &c, 1, &br);
      while(r == FR_OK && br) {
	xml_parse(c);      
	r = f_read(&fil, &c, 1, &br);
      }    
      f_close(&fil);

      // A file that held no menu -- empty, or not the XML it should be --
      // left the companion with nothing to set up, and it stopped right
      // there on an Atari ST. The core's own menu then.
      if(!cfg->menu) {
	debugf("%s holds no menu, taking the core's", sys_get_config_name());
	vPortFree(cfg);
	cfg = NULL;
      } else
	config_dump();
    }

    if(!cfg) {
      // no XML on SD card, try to load from core itself
      char *cfg_str = sys_get_config();
      if(cfg_str) {
	debugf("Loading XML config from core");
	config_init();
	char *c = cfg_str;
	while(*c) xml_parse(*c++); 
	config_dump();
      } else
	debugf("No valid config found, neither on sd card nor in core");
    }

    // process any pending interrupt. Filter out irq 1 which is the
    // FPGA cold boot event which we ignore since we just booted outselves
    sys_handle_interrupts(sys_irq_ctrl(0xff), true);
    
    // by default, DB9 interrupts are disabled. Reading
    // the DB9 state enables them. This is what hid_handle_event
    // does.
    hid_handle_event();

    // initialize on-screen-display and menu system
    osd_init();    
    menu_init();

#ifdef ENABLE_JTAG
    if(flash_test_result[0]) menu_draw_dialog_long("Flash test", flash_test_result);
#endif

    // open disk images, either defaults set in sdc_init or
    // user configure ones from the ini file. This will also
    // start rom image transfers if specified in the ini file
    sdc_mount_defaults();

    // finally run the ready action. This will usually get the core out of reset
    // On setups not using core configs, just release FPGA from reset
    // But this should actually never be the case nowadays.
    // TODO: An image upload may still be in progress ...
    // With a modem set up in the ini the net task does this a few seconds
    // later, once the ST's clock has been set, see netdl_hold_start().
    if(!sdc_image_upload_in_progress()) {
      if(!netdl_hold_start(core_ready)) core_ready();
      else debugf("Ready action waits for the clock from the modem");
    } else
      debugf("Image upload in progress, delaying ready action");

    ftpd_init();
    
    // finally prepare for wifi communication
    at_wifi_init();

    // and for the companion's own modem on port 1
    netdl_init();

    debugf("Entering main loop");
  
    for(;;) {
      mcu_hw_irq_ack();  // (re-)enable interrupt
      ulTaskNotifyTake( pdTRUE, portMAX_DELAY);    
      sys_handle_interrupts(sys_irq_ctrl(0xff), false);
    }
  }

  /* This will only be reached if the FPGA is not ready */
  /* So loop foreever while e.g. USB is still being handled */
  /* e.g. for debugging */
  for(;;) {
    // frequently check for an FPGA to show up and reboot to
    // startup normally if one is detected
    if(sys_status_is_valid()) {
      debugf("FPGA detected!");
      // This may be due to a USB download. So give USB
      // some time to finish. The same happens in sysconfig.c
      vTaskDelay(pdMS_TO_TICKS(2000));
      mcu_hw_reset();
    }
      
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

#ifdef ESP_PLATFORM
void app_main( void )
#else
int main( void )
#endif
{
  mcu_hw_init();
  telnetd_init();
  
  // run FPGA com thread
  // the BL616 build sets 2048 words, see its CMakeLists.txt
#ifndef COM_TASK_STACK
#define COM_TASK_STACK 4096
#endif
  xTaskCreate( com_task, "FPGA Com", COM_TASK_STACK, NULL, CONFIG_MAX_PRIORITY-1, &com_task_handle );

  mcu_hw_main_loop();

#ifndef ESP_PLATFORM
  return 0;
#endif
}
/*-----------------------------------------------------------*/

