#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

struct termios orig_termios;

void die(const char *s) {
  perror(s);
  exit(1);
}

void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1)
    die("tcsetattr");
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &orig_termios) == -1)
    die("tcgetattr");

  // We want to disable raw mode when the app is killed
  atexit(disableRawMode);

  struct termios raw = orig_termios;

  // Turn off a few local flags
  // 1. echo mode, i.e. what is typed by the users will not be printed
  //    on the screen
  // 2. canonical mode so that we don't read line-by-line, i.e. we get
  //    each char as it comes in
  // 3. SIGINT, i.e. prevent <ctrl-c> from terminating the program
  //    and SIGSTP, i.e. prevent <ctrl-z> from suspending the program
  // 4. <ctrl-v>, which will make the terminal wait for you to type another
  //    character then sends that character literally
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
  // Turn off some input flags
  // 1. XOFF and XON, i.e. <ctrl-s> and <ctrl-q> that pause and resume
  //    transmission of characters
  // 2. the automatic conversion of carriage returns into newlines
  // ... and some other probably obsolete stuff
  // 3. BRKINT, a break condition will cause a SIGINT signal to be sent to the
  //    program
  // 4. INPCK enables parity checking
  // 5. ISTRIP causes the 8th bit of each input byte to be stripped
  raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
  // Turn off output flags
  // 1. the conversion of newlines into a carriage return followed by a newline
  raw.c_oflag &= ~(OPOST);
  // Set the character size (CS) to 8 bits per byte
  raw.c_cflag |= (CS8);

  // Update the control characters (CC) to change terminal settings
  raw.c_cc[VMIN] = 0; // Min num of bytes before `read` can return
  // Max time before `read` returns, we set to 1/10th of a second
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    die("tcsetattr");
}

int main() {
  enableRawMode();

  while (1) {
    char c = '\0';

    if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN)
      die("read");

    // Since we turned of OPOST, we need to manually add carriage returns
    if (iscntrl(c)) {
      // Control characters can't be printed, so we just print their ASCII
      // value
      printf("%d\r\n", c);
    } else {
      printf("%d (%c)\r\n", c, c);
    }

    if (c == 'q')
      break;
  }

  return 0;
}
