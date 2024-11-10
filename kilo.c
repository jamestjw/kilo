/* Includes */

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* Defines */

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 3
#define DEBUG 0

// Strip the 5th and 6th bits from alpha characters to give us something
// between 1 - 26, i.e. the range of inputs of <ctrl-a> to <ctrl-z>
#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
  BACKSPACE = 127,
  // These need to be out of the range of normal chars
  ARROW_LEFT = 1000, /* \x1b[D */
  ARROW_RIGHT,       /* \x1b[C */
  ARROW_UP,          /* \x1b[A, */
  ARROW_DOWN,        /* \x1b[B */
  DEL_KEY,           /* <esc>[3~ */
  HOME_KEY,          /* <esc>[1~, <esc>[7~, <esc>[H, or <esc>OH */
  END_KEY,           /* <esc>[4~, <esc>[8~, <esc>[F, or <esc>OF */
  PAGE_UP,           /* <esc>[5~ */
  PAGE_DOWN,         /* <esc>[6~ */
};

/* Data */

typedef struct erow {
  int size;
  int rsize;
  char *chars;
  char *render;
} erow;

struct editorConfig {
  // Cursor coordinates within the open file, (0, 0) is the top left
  int cx, cy;
  // The x coordinates within the rendered string of the current row
  int rx;
  // Row offset, i.e. which row the user is scrolled to (1st visible row)
  int rowoff;
  // Column offset, i.e. which column the user is scrolled to (1st visible col)
  int coloff;
  int screenrows;
  int screencols;
  int numrows;
  erow *row;
  // Whether the file has unsaved modifications
  int dirty;
  char *filename;
  // The message we display in the status bar as well as when it was set
  char statusmsg[80];
  time_t statusmsg_time;
  struct termios orig_termios;
};

// Global struct containing editor state
struct editorConfig E;

/* Prototypes */
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));

/* Terminal */

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
  // 1. Echo mode, i.e. what is typed by the users will not be printed
  //    on the screen
  // 2. Canonical mode so that we don't read line-by-line, i.e. we get
  //    each char as it comes in
  // 3. SIGINT, i.e. prevent <ctrl-c> from terminating the program
  //    and SIGSTP, i.e. prevent <ctrl-z> from suspending the program
  // 4. <ctrl-v>, which will make the terminal wait for you to type another
  //    character then sends that character literally
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
  // Turn off some input flags
  // 1. XOFF and XON, i.e. <ctrl-s> and <ctrl-q> that pause and resume
  //    transmission of characters
  // 2. The automatic conversion of carriage returns into newlines
  // ... and some other probably obsolete stuff
  // 3. BRKINT, a break condition will cause a SIGINT signal to be sent to the
  //    program
  // 4. INPCK enables parity checking
  // 5. ISTRIP causes the 8th bit of each input byte to be stripped
  raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
  // Turn off output flags
  // 1. The conversion of newlines into a carriage return followed by a newline
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

int editorReadKey() {
  int nread;
  char c;

  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN)
      die("read");
    // If nread is 0, we keep going until we get something
  }

  // Escape character
  if (c == '\x1b') {
    char seq[3];

    // Check if we have enough keys in the escape sequence
    if (read(STDIN_FILENO, &seq[0], 1) != 1)
      return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1)
      return '\x1b';

    if (seq[0] == '[') {
      if ('0' <= seq[1] && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1)
          return '\x1b';
        // `PgUp` is sent as `<esc>[5~` and `PgDown` is sent as `<esc>[6~`
        if (seq[2] == '~') {
          switch (seq[1]) {
          case '1':
            return HOME_KEY;
          case '3':
            return DEL_KEY;
          case '4':
            return END_KEY;
          case '5':
            return PAGE_UP;
          case '6':
            return PAGE_DOWN;
          case '7':
            return HOME_KEY;
          case '8':
            return END_KEY;
          }
        }
      } else {
        switch (seq[1]) {

        // Up, down, left, right arrows are mapped to
        // \x1b[A, \x1b[B, \x1b[D, \x1b[C respectively
        // Alias arrow keys to `hjkl`
        case 'A':
          return ARROW_UP;
        case 'B':
          return ARROW_DOWN;
        case 'C':
          return ARROW_RIGHT;
        case 'D':
          return ARROW_LEFT;
        case 'H':
          return HOME_KEY;
        case 'F':
          return END_KEY;
        }
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
      case 'H':
        return HOME_KEY;
      case 'F':
        return END_KEY;
      }
    }

    // Assume user just hit `ESC`
    return '\x1b';
  } else {
    return c;
  }
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

/* row operations */

int editorRowCxToRx(erow *row, int cx) {
  int rx = 0;
  for (int j = 0; j < cx; j++) {
    if (row->chars[j] == '\t') {
      rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
    }
    rx++;
  }
  return rx;
}

int editorRowRxToCx(erow *row, int rx) {
  int cur_rx = 0;
  int cx;
  for (cx = 0; cx < row->size; cx++) {
    if (row->chars[cx] == '\t')
      cur_rx += (KILO_TAB_STOP - 1) - (cur_rx % KILO_TAB_STOP);
    cur_rx++;

    if (cur_rx > rx)
      return cx;
  }

  // In case the caller gives us an `rx` that's out of range
  return cx;
}

// Figures out what to render for each row and updates row->render.
// Also takes care of freeing what was previously there.
void editorUpdateRow(erow *row) {
  int tabs = 0;
  for (int j = 0; j < row->size; j++)
    if (row->chars[j] == '\t')
      tabs++;

  free(row->render);

  // The max number of characters we need for each tab is 8, hence we add 7
  // more for each tab.
  row->render = malloc(row->size + tabs * (KILO_TAB_STOP - 1) + 1);

  int idx = 0;
  for (int j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while (idx % KILO_TAB_STOP != 0)
        row->render[idx++] = ' ';
    } else {
      row->render[idx++] = row->chars[j];
    }
  }
  row->render[idx] = '\0';
  row->rsize = idx;
}

void editorInsertRow(int at, char *s, size_t len) {
  if (at < 0 || at > E.numrows)
    return;

  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  E.row[at].rsize = 0;
  E.row[at].render = NULL;

  editorUpdateRow(&E.row[at]);

  E.numrows++;
  // This gives us an idea of how dirty the file is since we aren't using
  // a boolean
  E.dirty++;
}

void editorFreeRow(erow *row) {
  free(row->render);
  free(row->chars);
}

void editorDelRow(int at) {
  if (at < 0 || at >= E.numrows)
    return;
  editorFreeRow(&E.row[at]);
  memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
  E.numrows--;
  E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
  // Allowing going one index more than the row size to allow appending to
  // a row
  if (at < 0 || at > row->size)
    at = row->size;

  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  // Update `render` and `rsize` fields
  editorUpdateRow(row);
  E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
  row->chars = realloc(row->chars, row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\0';
  editorUpdateRow(row);
  E.dirty++;
}

// Basically do the opposite of editorRowInsertChar
void editorRowDelChar(erow *row, int at) {
  if (at < 0 || at >= row->size)
    return;
  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
  row->size--;
  editorUpdateRow(row);
  E.dirty++;
}

/* Editor operations */

void editorInsertChar(int c) {
  // If the user is at the end of the file, we append a newline before
  // inserting the character
  if (E.cy == E.numrows)
    editorInsertRow(E.numrows, "", 0);

  // Insert the character and advance the cursor
  editorRowInsertChar(&E.row[E.cy], E.cx, c);
  E.cx++;
}

void editorInsertNewline() {
  if (E.cx == 0) {
    editorInsertRow(E.cy, "", 0);
  } else {
    erow *row = &E.row[E.cy];
    editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
    // Fetch again as editorInsertRow calls realloc
    row = &E.row[E.cy];
    row->size = E.cx;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
  }
  E.cy++;
  E.cx = 0;
}

void editorDelChar() {
  // Nothing to delete past the end of the file
  if (E.cy == E.numrows)
    return;
  // Check if we are at the beginning of the first line
  if (E.cx == 0 && E.cy == 0)
    return;

  // See if there is a character to the left of the cursor and delete it
  erow *row = &E.row[E.cy];
  if (E.cx > 0) {
    editorRowDelChar(row, E.cx - 1);
    E.cx--;
  } else {
    // We are at the start of some line
    E.cx = E.row[E.cy - 1].size;
    editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
    editorDelRow(E.cy);
    E.cy--;
  }
}

/* File i/o */

char *editorRowsToString(int *buflen) {
  int totlen = 0;
  for (int j = 0; j < E.numrows; j++) {
    // +1 for the newlines that we will add at the end of the file
    totlen += E.row[j].size + 1;
  }
  *buflen = totlen;

  char *buf = malloc(totlen);
  char *p = buf;
  for (int j = 0; j < E.numrows; j++) {
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    *p++ = '\n';
  }

  return buf;
}

void editorOpen(char *filename) {
  free(E.filename);
  E.filename = strdup(filename);

  FILE *fp = fopen(filename, "r");
  if (!fp)
    die("fopen");

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;

  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 &&
           (line[linelen - 1] == '\r' || line[linelen - 1] == '\n'))
      linelen--;
    editorInsertRow(E.numrows, line, linelen);
  }
  free(line);
  fclose(fp);
  E.dirty = 0;
}

void editorSave() {
  if (E.filename == NULL) {
    E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
    if (E.filename == NULL) {
      editorSetStatusMessage("Save aborted");
      return;
    }
  }

  int len;
  char *buf = editorRowsToString(&len);

  // FIXME: It would be much safer to write to a temporary file and rename it
  // to prevent data loss if saving doesn't succeed somehow.
  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
  if (fd != -1) {
    if (ftruncate(fd, len) != -1) {
      if (write(fd, buf, len) == len) {
        close(fd);
        free(buf);
        E.dirty = 0;
        editorSetStatusMessage("%d bytes written to disk", len);
        return;
      }
    }
    close(fd);
  }

  free(buf);
  editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/* Find */

void editorFindCallback(char *query, int key) {
  // Index of the row of the last match
  static int last_match = -1;
  // 1 for forwards and -1 for backwards
  static int direction = 1;

  if (key == '\r' || key == '\x1b') {
    // Reset to initial state for next call
    last_match = -1;
    direction = 1;
    return;
  } else if (key == ARROW_RIGHT || ARROW_DOWN) {
    direction = 1;
  } else if (key == ARROW_LEFT || key == ARROW_UP) {
    direction = -1;
  } else {
    last_match = -1;
    direction = 1;
  }

  if (last_match == -1)
    direction = 1;
  int current = last_match;

  for (int i = 0; i < E.numrows; i++) {
    current += direction;

    // We support wrapping around (both sides)
    if (current == -1)
      current = E.numrows - 1;
    else if (current == E.numrows)
      current = 0;

    erow *row = &E.row[current];
    char *match = strstr(row->render, query);

    if (match) {
      last_match = current;
      E.cy = current;
      E.cx = editorRowRxToCx(row, match - row->render);

      // A hack so that editorScroll will put the matching line at the top
      // at the next refresh
      E.rowoff = E.numrows;
      break;
    }
  }
}

void editorFind() {
  int saved_cx = E.cx;
  int saved_cy = E.cy;
  int saved_coloff = E.coloff;
  int saved_rowoff = E.rowoff;

  char *query =
      editorPrompt("Search: %s (Use ESC/Arrows/Enter)", editorFindCallback);
  if (query)
    free(query);
  else {
    // User pressed Esc if query is NULL
    E.cx = saved_cx;
    E.cy = saved_cy;
    E.coloff = saved_coloff;
    E.rowoff = saved_rowoff;
  }
}

/* Append buffer */

struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL)
    return;
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab) { free(ab->b); }

/* Output */

void editorScroll() {
  E.rx = 0;
  if (E.cy < E.numrows) {
    E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
  }

  // If the cursor is above the visible window, set the offset to where the
  // cursor is so that the cursor becomes visible
  if (E.cy < E.rowoff) {
    E.rowoff = E.cy;
  }

  // Check if cursor is below the visible window
  if (E.cy >= E.rowoff + E.screenrows) {
    E.rowoff = E.cy - E.screenrows + 1;
  }

  // Same as above code for vertical scrolling
  if (E.rx < E.coloff) {
    E.coloff = E.rx;
  }

  if (E.rx >= E.coloff + E.screencols) {
    E.coloff = E.rx - E.screencols + 1;
  }
}

// Should be called with the cursor at the top right
void editorDrawRows(struct abuf *ab) {
  // Drawing tildes on rows that aren't part of the file being edited
  for (int y = 0; y < E.screenrows; y++) {
    int filerow = y + E.rowoff;
    if (filerow >= E.numrows) {
      // Only print if user didn't open a file
      if (E.numrows == 0 && y == E.screenrows / 3) {
        char welcome[80];
        int len = snprintf(welcome, sizeof(welcome),
                           "Kilo editor -- version %s", KILO_VERSION);

        // Truncate welcome message if we don't have enough space
        if (len > E.screencols)
          len = E.screencols;

        // Centralise message with padding
        int padding = (E.screencols - len) / 2;

        if (padding) {
          abAppend(ab, "~", 1);
          padding--;
        }
        while (padding--)
          abAppend(ab, " ", 1);

        abAppend(ab, welcome, len);
      } else {
        abAppend(ab, "~", 1);
      }
    } else {
      int len = E.row[filerow].rsize - E.coloff;
      // In case the user scrolled off the end of the line
      if (len < 0)
        len = 0;
      else if (len > E.screencols)
        len = E.screencols;
      abAppend(ab, &E.row[filerow].render[E.coloff], len);
    }

    // K (erase in line) with the default argument 0, erases the part of the
    // line to the right of the cursor.
    abAppend(ab, "\x1b[K", 3);

    abAppend(ab, "\r\n", 2);
  }
}

void editorDrawStatusBar(struct abuf *ab) {
  // Use inverted colour formatting
  abAppend(ab, "\x1b[7m", 4);

  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
                     E.filename ? E.filename : "[No Name]", E.numrows,
                     E.dirty ? "(modified)" : "");
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy, E.numrows - 1);
  if (len > E.screencols) {
    len = E.screencols;
  }
  abAppend(ab, status, len);

  while (len < E.screencols) {
    if (E.screencols - len == rlen) {
      abAppend(ab, rstatus, rlen);
      break;
    }

    // Padding between left and right
    abAppend(ab, " ", 1);
    len++;
  }

  // Switch back to normal formatting
  abAppend(ab, "\x1b[m", 3);
  abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
  // Clear the message bar
  abAppend(ab, "\x1b[K", 3);

  int msglen = strlen(E.statusmsg);
  if (msglen > E.screencols) {
    msglen = E.screencols;
  }
  if (msglen && time(NULL) - E.statusmsg_time < 5)
    abAppend(ab, E.statusmsg, msglen);
}

// Escape sequences: https://vt100.net/docs/vt100-ug/chapter3.html
// \x1b - escape
// J    - erase in display
// 2    - clear entire screen (other options are 0 and 1, check docs)
void editorRefreshScreen() {
  editorScroll();

  struct abuf ab = ABUF_INIT;

  // Use the l command (reset mode) with argument `?25` to hide the cursor
  // while we redraw the screen.
  abAppend(&ab, "\x1b[?25l", 6);

  // The `H` command repositions the cursor, its default args place the cursor
  // at the top left, exactly where we want it.
  abAppend(&ab, "\x1b[H", 3);

  // Draw rows and status bar
  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);

  // Reposition cursor
  char buf[32];
  // The `H` command repositions the cursor and 1-indexed
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy - E.rowoff + 1,
           E.rx - E.coloff + 1);
  abAppend(&ab, buf, strlen(buf));

  // Use the h command (set mode) to restore the cursor
  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);

  // Get current time
  E.statusmsg_time = time(NULL);
}

/* Input */

char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
  size_t bufsize = 128;
  char *buf = malloc(bufsize);

  size_t buflen = 0;
  buf[0] = '\0';

  while (1) {
    editorSetStatusMessage(prompt, buf);
    editorRefreshScreen();

    int c = editorReadKey();

    if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
      if (buflen != 0)
        buf[--buflen] = '\0';
    } else if (c == '\x1b') {
      editorSetStatusMessage("");
      if (callback) {
        callback(buf, c);
      }
      free(buf);
      return NULL;
    } else if (c == '\r') {
      if (buflen != 0) {
        editorSetStatusMessage("");
        if (callback) {
          callback(buf, c);
        }
        return buf;
      }
    } else if (!iscntrl(c) && c < 128) {
      if (buflen == bufsize - 1) {
        bufsize *= 2;
        buf = realloc(buf, bufsize);
      }
      buf[buflen++] = c;
      buf[buflen] = '\0';
    }

    if (callback) {
      callback(buf, c);
    }
  }
}

void editorMoveCursor(int key) {
  // Row where the cursor currently is
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

  switch (key) {
  case ARROW_LEFT:
    if (E.cx != 0) {
      E.cx--;
    } else if (E.cy > 0) {
      // If the user is not on the first line and is at the far left of a line,
      // allow the user to go up to the previous line
      E.cy--;
      E.cx = E.row[E.cy].size;
    }
    break;
  case ARROW_RIGHT:
    if (row && E.cx < row->size) {
      E.cx++;
    } else if (row && E.cx == row->size) {
      // Allow the user to run off the end of a line to the next line if he
      // isn't already on the last row
      E.cy++;
      E.cx = 0;
    }
    break;
  case ARROW_UP:
    if (E.cy != 0)
      E.cy--;
    break;
  case ARROW_DOWN:
    // We can advance below the bottom of the screen, but not beyond the file
    if (E.cy < E.numrows)
      E.cy++;
    break;
  }

  // Get new row as we may have moved
  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  int rowlen = row ? row->size : 0;
  // If we moved from a longer line to a shorter line, ensure that the current
  // `cx` does not exceed the length of the current line
  if (E.cx > rowlen) {
    E.cx = rowlen;
  }
}

void editorProcessKeypress() {
  // Hacky way to determine if the user pressed <C-q> N-times in a row
  static int quit_times = KILO_QUIT_TIMES;

  int c = editorReadKey();

  switch (c) {
  case '\r': {
    editorInsertNewline();
    break;
  }

  case CTRL_KEY('q'): {
    if (E.dirty && quit_times > 0) {
      editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                             "Press Ctrl-Q %d more times to quit.",
                             quit_times);
      quit_times--;
      return;
    }
    // Refer to editorRefreshScreen for what these do
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
    break;
  }

  case CTRL_KEY('s'): {
    editorSave();
    break;
  }

  // Go to the left and right edges of the screen for now
  case HOME_KEY:
    E.cx = 0;
    break;

  case END_KEY:
    if (E.cy < E.numrows)
      E.cx = E.row[E.cy].size;
    break;

  case CTRL_KEY('f'): {
    editorFind();
    break;
  }

  case BACKSPACE:
  case CTRL_KEY('h'):
  case DEL_KEY: {
    // Quick hack to delete the character on the right of the cursor
    if (c == DEL_KEY)
      editorMoveCursor(ARROW_RIGHT);

    editorDelChar();
    break;
  }

  case PAGE_UP:
  case PAGE_DOWN: {
    // Put the cursor at the very top or bottom of the screen
    if (c == PAGE_UP) {
      E.cy = E.rowoff;
    } else if (c == PAGE_DOWN) {
      E.cy = E.rowoff + E.screenrows - 1;
      if (E.cy > E.numrows) {
        E.cy = E.numrows;
      }
    }
    // And move up/down an entire screen
    int times = E.screenrows;
    while (times--) {
      editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
    }
    break;
  }

  case ARROW_UP:
  case ARROW_DOWN:
  case ARROW_LEFT:
  case ARROW_RIGHT: {
    editorMoveCursor(c);
    break;
  }

  case CTRL_KEY('l'): // Refreshing the screen doesn't make sense
  case '\x1b':        // Ignore the escape key too
    break;

  default: {
    editorInsertChar(c);
    break;
  }
  }

  quit_times = KILO_QUIT_TIMES;
}

/* Init */

void initEditor() {
  E.cx = E.cy = E.rx = E.rowoff = E.coloff = E.numrows = E.dirty = 0;
  E.row = NULL;
  E.filename = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1)
    die("getWindowSize");

  // Reserve one line for the status bar
  E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();
  if (argc >= 2)
    editorOpen(argv[1]);

  editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}
