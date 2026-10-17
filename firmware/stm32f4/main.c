#include "hw.h"
#include "sys.h"
#include "uart.h"
#include "filter.h"
#include "stats.h"
#include "crc32.h"
#include "sensor.h"
#include "shell.h"

#define SAMPLE_PERIOD_MS 10

int main(void)
{
  char line[64];
  unsigned int last_sample;

  sys_init();
  systick_init(1000);
  uart_init();

  sensor_seed(0xC0FFEE01u);
  filter_init();
  stats_reset();
  (void)crc32((const unsigned char*)"", 0);   /* build CRC table up front */

  shell_banner();
  shell_prompt();

  led_on();
  delay_ms(150);
  led_off();

  last_sample = get_tick();
  for (;;) {
    if (shell_auto() && (get_tick() - last_sample) >= SAMPLE_PERIOD_MS) {
      int raw = sensor_read();
      int filt = filter_push(raw);
      stats_push(filt);
      last_sample = get_tick();
    }

    if (uart_available()) {
      int n = uart_getline(line, sizeof(line));
      if (n > 0) shell_exec(line);
      shell_prompt();
    }

    __asm__ volatile ("wfi");
  }
}
