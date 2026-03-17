/*
 * reboot-mode - reboot into a specific mode (bootloader, recovery, etc.)
 *
 * Usage: reboot-mode <mode>
 *   e.g. reboot-mode bootloader
 *        reboot-mode recovery
 *
 * Uses LINUX_REBOOT_CMD_RESTART2 with the mode string, which on
 * Qualcomm devices writes to the IMEM/PON scratch register so the
 * bootloader knows which mode to enter.
 */

#include <linux/reboot.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
  const char *mode;

  if (argc < 2) {
    fprintf(stderr, "Usage: %s <mode>\n", argv[0]);
    fprintf(stderr, "Modes: bootloader, recovery, edl\n");
    return 1;
  }

  mode = argv[1];

  sync();

  if (syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
              LINUX_REBOOT_CMD_RESTART2, mode) < 0) {
    perror("reboot");
    return 1;
  }

  /* should not reach here */
  return 0;
}
