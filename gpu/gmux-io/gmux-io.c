/*
 * gmux-io.c — read/write of apple-gmux registers (MacBookPro11,3, INDEXED type)
 * via /dev/port. Indexed protocol compatible with drivers/platform/x86/apple-gmux.c
 * (APPLE_GMUX_TYPE_INDEXED): PIO base 0x700, VALUE@0x7C2, READ@0x7D0, WRITE@0x7D4.
 *
 *   read8 : wait_ready -> outb(port, 0x7D0) -> wait_complete -> inb(0x7C2)
 *   write8: outb(val, 0x7C2) -> wait_ready -> outb(port, 0x7D4) -> wait_complete
 *
 * Usage:
 *   gmux-io read <reg>            8-bit read (hex)
 *   gmux-io read32 <reg>          32-bit read (hex)
 *   gmux-io write <reg> <val>     8-bit write (WARNING: live switch / power!)
 *   gmux-io dump                  full window 0x00-0xFF (read-only)
 *   gmux-io status                key registers + decoding
 *
 * Build: gcc -O2 -Wall -Wextra -o gmux-io gmux-io.c
 * Run:   sudo ./gmux-io status
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GMUX_BASE   0x700
#define PORT_VALUE  0xc2
#define PORT_READ   0xd0
#define PORT_WRITE  0xd4

/* registers (include/linux/apple-gmux.h) */
#define REG_VERSION_MAJOR       0x04
#define REG_VERSION_MINOR       0x05
#define REG_VERSION_RELEASE     0x06
#define REG_SWITCH_DISPLAY      0x10
#define REG_SWITCH_GET_DISPLAY  0x11
#define REG_INTERRUPT_ENABLE    0x14
#define REG_INTERRUPT_STATUS    0x16
#define REG_SWITCH_DDC          0x28
#define REG_SWITCH_EXTERNAL     0x40
#define REG_SWITCH_GET_EXTERNAL 0x41
#define REG_DISCRETE_POWER      0x50
#define REG_MAX_BRIGHTNESS      0x70
#define REG_BRIGHTNESS          0x74

static int fd = -1;

static uint8_t inb_p(uint16_t port)
{
	uint8_t v = 0;
	if (pread(fd, &v, 1, port) != 1) {
		fprintf(stderr, "pread(0x%03x): %s\n", port, strerror(errno));
		exit(1);
	}
	return v;
}

static void outb_p(uint16_t port, uint8_t val)
{
	if (pwrite(fd, &val, 1, port) != 1) {
		fprintf(stderr, "pwrite(0x%03x): %s\n", port, strerror(errno));
		exit(1);
	}
}

static uint32_t inl_p(uint16_t port)
{
	uint8_t buf[4];
	if (pread(fd, buf, 4, port) != 4) {
		fprintf(stderr, "pread32(0x%03x): %s\n", port, strerror(errno));
		exit(1);
	}
	return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
	       ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

/* wait until GWR bit0 == 0 (gmux ready for a new command) */
static int wait_ready(void)
{
	int i;
	for (i = 0; i < 2000; i++) {
		if (!(inb_p(GMUX_BASE + PORT_WRITE) & 0x01))
			return 1;
		usleep(100);
	}
	return 0;
}

/* wait until GWR bit0 == 1 (command finished), then drain READ */
static int wait_complete(void)
{
	int i;
	for (i = 0; i < 2000; i++) {
		if (inb_p(GMUX_BASE + PORT_WRITE) & 0x01) {
			inb_p(GMUX_BASE + PORT_READ);
			return 1;
		}
		usleep(100);
	}
	return 0;
}

static uint8_t gmux_read8(uint8_t port)
{
	uint8_t val;
	if (!wait_ready()) {
		fprintf(stderr, "gmux timeout (read8 ready)\n");
		exit(1);
	}
	outb_p(GMUX_BASE + PORT_READ, port);
	if (!wait_complete()) {
		fprintf(stderr, "gmux timeout (read8 complete)\n");
		exit(1);
	}
	val = inb_p(GMUX_BASE + PORT_VALUE);
	return val;
}

static uint32_t gmux_read32(uint8_t port)
{
	uint32_t val;
	if (!wait_ready()) {
		fprintf(stderr, "gmux timeout (read32 ready)\n");
		exit(1);
	}
	outb_p(GMUX_BASE + PORT_READ, port);
	if (!wait_complete()) {
		fprintf(stderr, "gmux timeout (read32 complete)\n");
		exit(1);
	}
	val = inl_p(GMUX_BASE + PORT_VALUE);
	return val;
}

static void gmux_write8(uint8_t port, uint8_t val)
{
	outb_p(GMUX_BASE + PORT_VALUE, val);
	if (!wait_ready()) {
		fprintf(stderr, "gmux timeout (write8 ready)\n");
		exit(1);
	}
	outb_p(GMUX_BASE + PORT_WRITE, port);
	if (!wait_complete()) {
		fprintf(stderr, "gmux timeout (write8 complete)\n");
		exit(1);
	}
}

static void gmux_write32(uint8_t port, uint32_t val)
{
	int i;
	for (i = 0; i < 4; i++)
		outb_p(GMUX_BASE + PORT_VALUE + (uint16_t)i,
		       (uint8_t)(val >> (8 * i)));
	if (!wait_ready()) {
		fprintf(stderr, "gmux timeout (write32 ready)\n");
		exit(1);
	}
	outb_p(GMUX_BASE + PORT_WRITE, port);
	if (!wait_complete()) {
		fprintf(stderr, "gmux timeout (write32 complete)\n");
		exit(1);
	}
}

static unsigned long parse_reg(const char *s)
{
	unsigned long v;
	char *end;
	errno = 0;
	v = strtoul(s, &end, 0);
	if (errno || *end || v > 0xff) {
		fprintf(stderr, "bad register: %s (0x00-0xff)\n", s);
		exit(2);
	}
	return v;
}

static unsigned long parse_val(const char *s)
{
	unsigned long v;
	char *end;
	errno = 0;
	v = strtoul(s, &end, 0);
	if (errno || *end || v > 0xff) {
		fprintf(stderr, "bad value: %s (0x00-0xff)\n", s);
		exit(2);
	}
	return v;
}

static void cmd_read(int argc, char **argv)
{
	if (argc < 1) {
		fprintf(stderr, "usage: gmux-io read <reg>\n");
		exit(2);
	}
	printf("0x%02x\n", gmux_read8((uint8_t)parse_reg(argv[0])));
}

static void cmd_read32(int argc, char **argv)
{
	if (argc < 1) {
		fprintf(stderr, "usage: gmux-io read32 <reg>\n");
		exit(2);
	}
	printf("0x%08x\n", gmux_read32((uint8_t)parse_reg(argv[0])));
}

static void cmd_write(int argc, char **argv)
{
	unsigned long reg, val;
	if (argc < 2) {
		fprintf(stderr, "usage: gmux-io write <reg> <val>\n");
		exit(2);
	}
	reg = parse_reg(argv[0]);
	val = parse_val(argv[1]);
	gmux_write8((uint8_t)reg, (uint8_t)val);
	printf("0x%02x <- 0x%02x\n", (unsigned)reg, (unsigned)val);
}

static void cmd_write32(int argc, char **argv)
{
	unsigned long reg;
	unsigned long val;
	char *end;
	if (argc < 2) {
		fprintf(stderr, "usage: gmux-io write32 <reg> <val32>\n");
		exit(2);
	}
	reg = parse_reg(argv[0]);
	errno = 0;
	val = strtoul(argv[1], &end, 0);
	if (errno || *end || val > 0xffffffffUL) {
		fprintf(stderr, "bad value: %s (0x00000000-0xffffffff)\n", argv[1]);
		exit(2);
	}
	gmux_write32((uint8_t)reg, (uint32_t)val);
	printf("0x%02x <- 0x%08lx\n", (unsigned)reg, val);
}

static void cmd_dump(void)
{
	int off;
	printf("# gmux register window 0x00-0xff (read-only, indexed)\n");
	for (off = 0; off < 0x100; off++)
		printf("0x%02x\t0x%02x\n", off, gmux_read8((uint8_t)off));
}

static const char *display_state(uint8_t v)
{
	return (v & 0x01) ? "DIS (dGPU)" : "IGD (iGPU)";
}

static const char *ddc_state(uint8_t v)
{
	return (v == 1) ? "IGD (iGPU)" : "DIS (dGPU)";
}

static const char *external_state(uint8_t v)
{
	return (v == 2) ? "IGD (iGPU)" : "DIS (dGPU)";
}

static const char *power_state(uint8_t v)
{
	switch (v) {
	case 0x00: return "OFF";
	case 0x01: return "ON (transition)";
	case 0x03: return "ON (latch)";
	default:   return "unknown";
	}
}

static void cmd_status(void)
{
	uint8_t v;
	uint32_t b;

	printf("# gmux status (MBP11,3 indexed, base 0x%03x)\n", GMUX_BASE);

	v = gmux_read8(REG_SWITCH_DISPLAY);
	printf("SWITCH_DISPLAY  0x10 = 0x%02x  -> %s\n", v, display_state(v));
	v = gmux_read8(REG_SWITCH_GET_DISPLAY);
	printf("GET_DISPLAY     0x11 = 0x%02x  -> %s\n", v, display_state(v));
	v = gmux_read8(REG_SWITCH_DDC);
	printf("SWITCH_DDC      0x28 = 0x%02x  -> %s\n", v, ddc_state(v));
	v = gmux_read8(REG_SWITCH_EXTERNAL);
	printf("SWITCH_EXTERNAL 0x40 = 0x%02x  -> %s\n", v, external_state(v));
	v = gmux_read8(REG_SWITCH_GET_EXTERNAL);
	printf("GET_EXTERNAL    0x41 = 0x%02x  -> %s\n", v, external_state(v));
	v = gmux_read8(REG_DISCRETE_POWER);
	printf("DISCRETE_POWER  0x50 = 0x%02x  -> %s\n", v, power_state(v));
	v = gmux_read8(REG_INTERRUPT_ENABLE);
	printf("INTERRUPT_EN    0x14 = 0x%02x\n", v);
	v = gmux_read8(REG_INTERRUPT_STATUS);
	printf("INTERRUPT_STAT  0x16 = 0x%02x\n", v);
	b = gmux_read32(REG_MAX_BRIGHTNESS);
	printf("MAX_BRIGHTNESS  0x70 = 0x%08x (%u)\n", b, b);
	b = gmux_read32(REG_BRIGHTNESS);
	printf("BRIGHTNESS      0x74 = 0x%08x (%u)\n", b, b);
}

static void usage(const char *prog)
{
	printf("Usage: %s <command> [args]\n", prog);
	printf("  read <reg>      8-bit read (hex)\n");
	printf("  read32 <reg>    32-bit read (hex)\n");
	printf("  write <reg> <v> 8-bit write (WARNING: live switch / power!)\n");
	printf("  write32 <reg> <v> 32-bit write (WARNING: live switch / power!)\n");
	printf("  dump            full window 0x00-0xff (read-only)\n");
	printf("  status          key registers + decoding\n");
}

int main(int argc, char **argv)
{
	const char *cmd;

	if (argc < 2) {
		usage(argv[0]);
		return 2;
	}
	cmd = argv[1];

	/* validate the command and argument count BEFORE opening /dev/port */
	if (strcmp(cmd, "read") == 0 || strcmp(cmd, "read32") == 0) {
		if (argc < 3) {
			fprintf(stderr, "usage: gmux-io %s <reg>\n", cmd);
			return 2;
		}
	} else if (strcmp(cmd, "write") == 0 || strcmp(cmd, "write32") == 0) {
		if (argc < 4) {
			fprintf(stderr, "usage: gmux-io %s <reg> <val>\n", cmd);
			return 2;
		}
	} else if (strcmp(cmd, "dump") == 0 || strcmp(cmd, "status") == 0) {
		/* no arguments */
	} else {
		fprintf(stderr, "unknown command: %s\n", cmd);
		usage(argv[0]);
		return 2;
	}

	fd = open("/dev/port", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open /dev/port: %s (root required)\n",
			strerror(errno));
		return 1;
	}

	if (strcmp(cmd, "read") == 0)
		cmd_read(argc - 2, argv + 2);
	else if (strcmp(cmd, "read32") == 0)
		cmd_read32(argc - 2, argv + 2);
	else if (strcmp(cmd, "write") == 0)
		cmd_write(argc - 2, argv + 2);
	else if (strcmp(cmd, "write32") == 0)
		cmd_write32(argc - 2, argv + 2);
	else if (strcmp(cmd, "dump") == 0)
		cmd_dump();
	else if (strcmp(cmd, "status") == 0)
		cmd_status();

	close(fd);
	return 0;
}