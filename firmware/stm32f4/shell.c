#include "hw.h"
#include "sys.h"
#include "uart.h"
#include "filter.h"
#include "stats.h"
#include "crc32.h"
#include "sensor.h"
#include "shell.h"

static int auto_sample = 1;

static int str_eq(const char* a, const char* b)
{
  while (*a && *a == *b) { a++; b++; }
  return *a == *b;
}

static int hexval(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int tokenize(char* s, char* argv[], int max)
{
  int argc = 0;
  while (*s && argc < max) {
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) break;
    argv[argc++] = s;
    while (*s && *s != ' ' && *s != '\t') s++;
    if (*s) { *s = 0; s++; }
  }
  return argc;
}

static int parse_int(const char* s, int* out)
{
  int neg = 0, val = 0, any = 0;
  if (*s == '-') { neg = 1; s++; }
  while (*s >= '0' && *s <= '9') { val = val * 10 + (*s - '0'); s++; any = 1; }
  if (!any || *s) return 0;
  *out = neg ? -val : val;
  return 1;
}

static int parse_hex(const char* s, unsigned int* out)
{
  unsigned int val = 0;
  int any = 0, d;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
  while ((d = hexval(*s)) >= 0) { val = val * 16u + (unsigned int)d; s++; any = 1; }
  if (!any || *s) return 0;
  *out = val;
  return 1;
}

int shell_auto(void) { return auto_sample; }

void shell_banner(void)
{
  uart_puts("\r\n");
  uart_puts("========================================\r\n");
  uart_puts("  Scourge STM32F407 bare-metal firmware  \r\n");
  uart_puts("  sensor -> MA filter -> stats / CRC32   \r\n");
  uart_puts("  type 'help' for commands               \r\n");
  uart_puts("========================================\r\n");
}

void shell_prompt(void)
{
  uart_puts("fw> ");
}

static void cmd_help(void)
{
  uart_puts("commands:\r\n");
  uart_puts("  help            show this list\r\n");
  uart_puts("  info            firmware + uptime\r\n");
  uart_puts("  sensor on|off   toggle live sampling\r\n");
  uart_puts("  run <n>         take n live sensor samples\r\n");
  uart_puts("  sample <v>      inject value v into filter+stats\r\n");
  uart_puts("  filter <n>      set moving-average window (1..64)\r\n");
  uart_puts("  stats           print count/min/max/mean/var/std\r\n");
  uart_puts("  reset           reset filter + statistics\r\n");
  uart_puts("  crc <hex>       CRC32 of hex bytes (crc 313233343536373839)\r\n");
  uart_puts("  selftest        run built-in tests\r\n");
  uart_puts("  led on|off      user LED (GPIOD12)\r\n");
  uart_puts("  peek <hex>      read memory word\r\n");
  uart_puts("  poke <hex> <h>  write memory word\r\n");
}

static void cmd_info(void)
{
  uart_puts("firmware : scourge-stm32f4 v1.0\r\n");
  uart_puts("mcu      : STM32F407 Cortex-M4 @ 16MHz\r\n");
  uart_puts("window   : ");
  uart_puti(filter_window());
  uart_puts("\r\n");
  uart_puts("uptime   : ");
  uart_putu(get_tick());
  uart_puts(" ms\r\n");
}

static void cmd_stats(void)
{
  uart_puts("n=");    uart_puti(stats_count());
  uart_puts(" min="); uart_puti(stats_min());
  uart_puts(" max="); uart_puti(stats_max());
  uart_puts(" mean=");uart_puti(stats_mean());
  uart_puts(" var="); uart_puti((int)stats_variance());
  uart_puts(" std="); uart_puti(stats_stddev());
  uart_puts("\r\n");
}

static void cmd_crc(const char* hex)
{
  unsigned char buf[64];
  int n = 0;
  while (hex[0] && hex[1] && n < (int)sizeof(buf)) {
    int hi = hexval(hex[0]);
    int lo = hexval(hex[1]);
    if (hi < 0 || lo < 0) break;
    buf[n++] = (unsigned char)((hi << 4) | lo);
    hex += 2;
  }
  uart_puts("len=");
  uart_puti(n);
  uart_puts(" crc32=0x");
  uart_puthex(crc32(buf, (unsigned int)n), 8);
  uart_puts("\r\n");
}

static void cmd_selftest(void)
{
  int pass = 0, total = 0;

  {
    unsigned int c = crc32((const unsigned char*)"123456789", 9);
    int ok = (c == 0xCBF43926u);
    total++; pass += ok;
    uart_puts("[crc32 \"123456789\"] =0x"); uart_puthex(c, 8);
    uart_puts(ok ? " PASS\r\n" : " FAIL\r\n");
  }
  {
    unsigned int c = crc32((const unsigned char*)"", 0);
    int ok = (c == 0u);
    total++; pass += ok;
    uart_puts("[crc32 \"\"]        =0x"); uart_puthex(c, 8);
    uart_puts(ok ? " PASS\r\n" : " FAIL\r\n");
  }
  {
    int a;
    filter_reset(); filter_set_window(4);
    filter_push(100); filter_push(200); filter_push(300);
    a = filter_push(400);
    {
      int ok = (a == 250);
      total++; pass += ok;
      uart_puts("[ma4 100/200/300/400] ="); uart_puti(a);
      uart_puts(ok ? " PASS\r\n" : " FAIL\r\n");
    }
  }
  {
    int r = isqrt(144);
    int ok = (r == 12);
    total++; pass += ok;
    uart_puts("[isqrt 144]        ="); uart_puti(r);
    uart_puts(ok ? " PASS\r\n" : " FAIL\r\n");
  }
  {
    int i, sd, mean, ok;
    stats_reset();
    for (i = 0; i < 50; i++) stats_push(500);
    sd = stats_stddev();
    mean = stats_mean();
    ok = (sd == 0 && mean == 500);
    total++; pass += ok;
    uart_puts("[stats const 500]  mean="); uart_puti(mean);
    uart_puts(" std="); uart_puti(sd);
    uart_puts(ok ? " PASS\r\n" : " FAIL\r\n");
  }

  uart_puts("selftest: ");
  uart_puti(pass); uart_puts("/"); uart_puti(total);
  uart_puts(pass == total ? " ALL PASS\r\n" : " SOME FAIL\r\n");

  filter_reset(); filter_set_window(8);
  stats_reset();
}

void shell_exec(char* line)
{
  char* argv[8];
  int argc = tokenize(line, argv, 8);
  const char* cmd;

  if (argc == 0) return;
  cmd = argv[0];

  if (str_eq(cmd, "help")) cmd_help();
  else if (str_eq(cmd, "info")) cmd_info();
  else if (str_eq(cmd, "stats")) cmd_stats();
  else if (str_eq(cmd, "reset")) { filter_reset(); stats_reset(); uart_puts("reset ok\r\n"); }
  else if (str_eq(cmd, "sensor")) {
    if (argc >= 2 && str_eq(argv[1], "on"))  { auto_sample = 1; uart_puts("sensor on\r\n"); }
    else if (argc >= 2 && str_eq(argv[1], "off")) { auto_sample = 0; uart_puts("sensor off\r\n"); }
    else uart_puts("usage: sensor on|off\r\n");
  }
  else if (str_eq(cmd, "sample")) {
    int v;
    if (argc >= 2 && parse_int(argv[1], &v)) {
      int f = filter_push(v);
      stats_push(f);
      uart_puts("raw="); uart_puti(v);
      uart_puts(" filt="); uart_puti(f);
      uart_puts("\r\n");
    } else uart_puts("usage: sample <value>\r\n");
  }
  else if (str_eq(cmd, "run")) {
    int n = 1, i;
    if (argc >= 2) parse_int(argv[1], &n);
    if (n < 0) n = 0;
    for (i = 0; i < n; i++) {
      int raw = sensor_read();
      int f = filter_push(raw);
      stats_push(f);
    }
    uart_puts("ran "); uart_puti(n); uart_puts(" samples\r\n");
  }
  else if (str_eq(cmd, "filter")) {
    int w;
    if (argc >= 2 && parse_int(argv[1], &w)) {
      filter_set_window(w);
      uart_puts("window="); uart_puti(filter_window()); uart_puts("\r\n");
    } else uart_puts("usage: filter <n>\r\n");
  }
  else if (str_eq(cmd, "crc")) {
    if (argc >= 2) cmd_crc(argv[1]);
    else uart_puts("usage: crc <hexbytes>\r\n");
  }
  else if (str_eq(cmd, "selftest")) cmd_selftest();
  else if (str_eq(cmd, "led")) {
    if (argc >= 2 && str_eq(argv[1], "on"))  { led_on();  uart_puts("led on\r\n"); }
    else if (argc >= 2 && str_eq(argv[1], "off")) { led_off(); uart_puts("led off\r\n"); }
    else uart_puts("usage: led on|off\r\n");
  }
  else if (str_eq(cmd, "peek")) {
    unsigned int a;
    if (argc >= 2 && parse_hex(argv[1], &a)) {
      uart_puts("[0x"); uart_puthex(a, 8); uart_puts("] = 0x");
      uart_puthex(*(volatile unsigned int*)a, 8); uart_puts("\r\n");
    } else uart_puts("usage: peek <hexaddr>\r\n");
  }
  else if (str_eq(cmd, "poke")) {
    unsigned int a, v;
    if (argc >= 3 && parse_hex(argv[1], &a) && parse_hex(argv[2], &v)) {
      *(volatile unsigned int*)a = v;
      uart_puts("ok\r\n");
    } else uart_puts("usage: poke <hexaddr> <hexval>\r\n");
  }
  else {
    uart_puts("unknown: "); uart_puts(cmd); uart_puts("  (type help)\r\n");
  }
}
