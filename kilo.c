#include <stdlib.h>
#include <unistd.h>
#include <termios.h>


struct termios orig_termios;

void disableRawMode() {
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enableRawMode() {
  tcgetattr(STDIN_FILENO, &orig_termios);
  atexit(disableRawMode);

  struct termios raw = orig_termios;

  // Turn off echo mode, i.e. what is typed by the users will not be printed
  // on the screen.
  // Turn off canonical mode so that we don't read line-by-line, i.e. we get
  // each char as it comes in
  raw.c_lflag &= ~(ECHO | ICANON);

  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int main() {
  enableRawMode();

  char c;

  while (read(STDIN_FILENO, &c, 1) == 1 && c != 'q')
    ;

  return 0;
}
