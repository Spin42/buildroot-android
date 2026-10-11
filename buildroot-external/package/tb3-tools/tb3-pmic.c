/*
 * tb3-pmic - MT6323 PMIC access through the MT8127 PMIC wrapper (/dev/mem), and
 * reboot-mode for the bootloader (LK) of the Lenovo TB3-710F.
 *
 *   tb3-pmic read  <reg> [count]      read 16-bit PMIC registers (reg is the byte address)
 *   tb3-pmic write <reg> <val>
 *   tb3-pmic reboot-mode <bootloader|recovery|normal>
 *
 * LK picks fastboot/recovery from the RTC power-down register PDN1 (PMIC 0x802c):
 * bit 13 = fast boot, bit 4 = recovery. Same bits as the vendor rtc_mark_*() helpers.
 * The wrapper has to be initialised already (the preloader does it).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <unistd.h>

#define PWRAP_PHYS	0x1000D000UL
#define WACS2_EN	0x94
#define INIT_DONE2	0x98
#define WACS2_CMD	0x9c
#define WACS2_RDATA	0xa0
#define WACS2_VLDCLR	0xa4
#define FSM_IDLE	0
#define FSM_WFVLDCLR	6

#define RTC_BASE	0x8000
#define RTC_BBPU	(RTC_BASE + 0x00)
#define RTC_BBPU_CBUSY	(1 << 6)
#define RTC_PDN1	(RTC_BASE + 0x2c)
#define RTC_PROT	(RTC_BASE + 0x36)
#define RTC_WRTGR	(RTC_BASE + 0x3c)
#define PDN1_RECOVERY	(0x3 << 4)
#define PDN1_FAC_RESET	(1 << 4)
#define PDN1_FAST_BOOT	(1 << 13)

static volatile uint32_t *pw;

static uint32_t rd(unsigned off) { return pw[off / 4]; }
static void wr(unsigned off, uint32_t v) { pw[off / 4] = v; __sync_synchronize(); }

static int wait_fsm(int want)
{
	for (long i = 0; i < 2000000; i++)
		if (((rd(WACS2_RDATA) >> 16) & 7) == want)
			return 0;
	return -1;
}

static int pmic_read(unsigned reg, uint16_t *val)
{
	if (wait_fsm(FSM_IDLE)) return -1;
	wr(WACS2_CMD, (reg >> 1) << 16);
	if (wait_fsm(FSM_WFVLDCLR)) return -1;
	*val = rd(WACS2_RDATA) & 0xffff;
	wr(WACS2_VLDCLR, 1);
	return 0;
}

static int pmic_write(unsigned reg, uint16_t val)
{
	if (wait_fsm(FSM_IDLE)) return -1;
	wr(WACS2_CMD, (1u << 31) | ((reg >> 1) << 16) | val);
	return 0;
}

static int rtc_trigger(void)
{
	uint16_t v;
	if (pmic_write(RTC_WRTGR, 1)) return -1;
	for (int i = 0; i < 100000; i++) {
		if (pmic_read(RTC_BBPU, &v)) return -1;
		if (!(v & RTC_BBPU_CBUSY)) return 0;
	}
	return -1;
}

static int rtc_unlock(void)
{
	if (pmic_write(RTC_PROT, 0x586a) || rtc_trigger()) return -1;
	if (pmic_write(RTC_PROT, 0x9136) || rtc_trigger()) return -1;
	return 0;
}

static int reboot_mode(const char *mode)
{
	uint16_t pdn1;

	if (rtc_unlock() || pmic_read(RTC_PDN1, &pdn1)) { fprintf(stderr, "PMIC access failed\n"); return 1; }
	pdn1 &= ~(PDN1_RECOVERY | PDN1_FAST_BOOT);
	if (!strcmp(mode, "bootloader") || !strcmp(mode, "fastboot"))
		pdn1 |= PDN1_FAST_BOOT;
	else if (!strcmp(mode, "recovery"))
		pdn1 |= PDN1_FAC_RESET;
	else if (strcmp(mode, "normal")) { fprintf(stderr, "unknown mode '%s'\n", mode); return 1; }
	if (pmic_write(RTC_PDN1, pdn1) || rtc_trigger()) { fprintf(stderr, "PMIC write failed\n"); return 1; }
	pmic_read(RTC_PDN1, &pdn1);
	printf("PDN1 = 0x%04x, rebooting into %s\n", pdn1, mode);
	fflush(stdout);
	sync();
	reboot(RB_AUTOBOOT);
	return 0;
}

int main(int argc, char **argv)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	uint16_t v;

	if (fd < 0) { perror("/dev/mem"); return 1; }
	pw = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, PWRAP_PHYS);
	if (pw == MAP_FAILED) { perror("mmap"); return 1; }
	if (argc < 2) goto usage;

	if (!strcmp(argv[1], "status")) {
		printf("INIT_DONE2=0x%x WACS2_EN=0x%x RDATA=0x%08x\n", rd(INIT_DONE2), rd(WACS2_EN), rd(WACS2_RDATA));
		return 0;
	}
	if (!strcmp(argv[1], "read") && argc >= 3) {
		unsigned reg = strtoul(argv[2], NULL, 0), n = argc > 3 ? strtoul(argv[3], NULL, 0) : 1;
		for (unsigned i = 0; i < n; i++, reg += 2) {
			if (pmic_read(reg, &v)) { fprintf(stderr, "read 0x%04x failed\n", reg); return 1; }
			printf("0x%04x: 0x%04x\n", reg, v);
		}
		return 0;
	}
	if (!strcmp(argv[1], "write") && argc == 4)
		return pmic_write(strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0)) ? 1 : 0;
	if (!strcmp(argv[1], "reboot-mode") && argc == 3)
		return reboot_mode(argv[2]);
usage:
	fprintf(stderr, "usage: %s status | read <reg> [n] | write <reg> <val> | reboot-mode <bootloader|recovery|normal>\n", argv[0]);
	return 1;
}
