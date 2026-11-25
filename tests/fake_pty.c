#include "fake_device.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

int main(void) {
  fake_init_data();
  int master = posix_openpt(O_RDWR | O_NOCTTY);
  if (master < 0 || grantpt(master) || unlockpt(master)) {
    perror("posix_openpt");
    return 1;
  }
  const char *path = ptsname(master);
  int slave = open(path, O_RDWR | O_NOCTTY);
  struct termios tio;
  if (slave < 0 || tcgetattr(slave, &tio) != 0) {
    perror(path);
    return 1;
  }
  cfmakeraw(&tio);
  tcsetattr(slave, TCSANOW, &tio);

  fake_options_t options = {.sdk = true, .platform = 0x20};
  pid_t pid = fake_spawn(master, slave, &options);
  printf("%s\n", path);
  fflush(stdout);

  char c;
  while (read(STDIN_FILENO, &c, 1) > 0) {
  }
  kill(pid, SIGTERM);
  waitpid(pid, NULL, 0);
  return 0;
}
