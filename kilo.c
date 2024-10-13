/* includes */

#include <asm-generic/ioctls.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* defines */

#define DEBUG 0

// Strip the 5th and 6th bits from alpha characters to give us something
// between 1 - 26, i.e. the range of inputs of <ctrl-a> to <ctrl-z>
#define CTRL_KEY(k) ((k) & 0x1f)

/* data */

struct editorConfig {
  int screenrows;
  int screencols;
  struct termios orig_termios;
};

// Global struct containing editor state
struct editorConfig E;

/* terminal */

void die(const char *s) {
  // Refer to editorRefreshScreen for what these do
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  perror(s);
  exit(1);
}

void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die("tcsetattr");
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
    die("tcgetattr");

  // We want to disable raw mode when the app is killed
  atexit(disableRawMode);

  struct termios raw = E.orig_termios;

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

char editorReadKey() {
  int nread;
  char c;

  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN)
      die("read");
    // If nread is 0, we keep going until we get something
  }

  if (DEBUG) {
    printf("Read key: ");
    // Since we turned of OPOST, we need to manually add carriage returns
    if (iscntrl(c)) {
      // Control characters can't be printed, so we just print their ASCII
      // value
      printf("%d\r\n", c);
    } else {
      printf("%d (%c)\r\n", c, c);
    }
  }

  return c;
}

int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;
  // Use the device status report `n` command with an argument of 6 to get the
  // current cursor position
  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
    return -1;

  // We expect to be able to read an input of the format \x1b[24;80R or
  // something of this format, where 24 is the height and 80 is the width
  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1)
      break;
    if (buf[i] == 'R')
      break;
    i++;
  }
  // Conveniently replace the `R` with a null-byte
  buf[i] = '\0';

  if (buf[0] != '\x1b' || buf[1] != '[')
    return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
    return -1;

  return 0;
}

// Get screen height and width
int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  // TIOCGWINSZ stands for Terminal IOCtl Get WINdow SiZe
  if (1 || ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    // Fallback method to get window size, we move the cursor to the bottom
    // right, i.e. 999 columns (C) to the right, and 999 columns down (B)
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
      return -1;
    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/* output */

void editorDrawRows() {
  // Drawing tildes on rows that aren't part of the file being edited
  for (int y = 0; y < E.screenrows; y++) {
    write(STDOUT_FILENO, "~", 1);

    // When we get to the very last row, we don't want to print a newline as
    // this causes the screen to scroll
    if (y < E.screenrows - 1) {
      write(STDOUT_FILENO, "\r\n", 2);
    }
  }
}

// Escape sequences: https://vt100.net/docs/vt100-ug/chapter3.html
// \x1b - escape
// J    - erase in display
// 2    - clear entire screen (other options are 0 and 1, check docs)
void editorRefreshScreen() {
  // This erases the screen and places the cursor at the bottom left
  write(STDOUT_FILENO, "\x1b[2J", 4);
  // The `H` command repositions the cursor, its default args place the cursor
  // at the top left, exactly where we want it.
  write(STDOUT_FILENO, "\x1b[H", 3);

  // Draw rows and reposition cursor
  editorDrawRows();
  write(STDOUT_FILENO, "\x1b[H", 3);
}

/* input */

void editorProcessKeypress() {
  char c = editorReadKey();

  switch (c) {
  case CTRL_KEY('q'): {
    // Refer to editorRefreshScreen for what these do
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
    break;
  }
  }
}

/* init */

void initEditor() {
  if (getWindowSize(&E.screenrows, &E.screencols) == -1)
    die("getWindowSize");
}

int main() {
  enableRawMode();
  initEditor();

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}
