#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE


#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <ctype.h> 
#include <stdio.h> 
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <stdarg.h>
#include <fcntl.h>


#define CTRL_KEY(k) ((k) & 0x1f)
#define EDITOR_VERSION "1.0"
#define EDITOR_TAB_STOP 8
#define EDITOR_QUIT_TIMES 3 

//home_key = start of line, end_key = end of line
enum editorKey {
  BACKSPACE = 127,
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

// store location for text row in editor 
typedef struct erow {
  int size;
  int rsize; 
  char *chars;
  char *render; 
} erow;

//raw mode is neccessary so text editor can continue accepting input 
//terminal attributes read into a termios struct
struct editorConfig{
  int cursor_x; 
  int cursor_y; 
  int rx; 
  int rowoffset; 
  int coloffset; 
  int screen_rows;
  int screen_cols;
  //keeps track of unsaved changes, dirty flag 
  int dirty; 
  int numrows; 
  erow *row; 
  char *filename;
  char statusmsg[80];
  time_t statusmsg_time;
  struct termios original_term;
};

struct editorConfig E;

//prototype for status message
void editorSetStatusMessage(const char *fmt, ...);
//prototype for refreshing editor screen
void editorRefreshScreen();
char *editorPrompt(char *prompt);

//terminal functions

//force exit on program upon error
void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);
  perror(s);
  exit(1);
}

void disableRawMode() {
  if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.original_term) == -1)
    die("tcsetattr failed");
}

void enableRawMode() {
  if(tcgetattr(STDIN_FILENO, &E.original_term) == -1)
  {
    die("tcgetattr failed"); 
  }
  atexit(disableRawMode);
  struct termios raw = E.original_term;
   //turning off ECHO to prevent inputted char from appearing on terminal (and with bitwise-NOT since ECHO is bit flag) 
   //reading by bytes now with ICANON flag (instead of by line)
   //turn off ctrl-z or ctrl-c escape through ISIG flag
   //disable ctrl-v through IEXTEN flag
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
  //disable Ctrl-S and Ctrl-Q controls on data flow  
  //turn off alternative enter input from ctrl-m through IXON flag
  //make sure ctrl-c sigint is truly off with brkint, and enable parity checking (INPCK)
  raw.c_iflag &= ~(BRKINT | ISTRIP | INPCK | ICRNL | IXON);
  //disable "/r" to "/r/n" translation
  raw.c_oflag &= ~(OPOST);
  //make sure char has 8 bits per byte formatting
  raw.c_cflag |= (CS8);
  //min number of bytes of input needed for read() can return 
  //set to 0 so that read() returns as soon as there is any input to be read
  raw.c_cc[VMIN] = 0;
  //vtime value sets max amount of time to wait before read() returns (1 = 1/10 of a second)
  //if there is no input for a period of time, should see VMIN value being printed out to the console every 10th of a second
  raw.c_cc[VTIME] = 1;

  //TCSAFlush discards additional input 
  if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
  {
    die("tcsetattr failed"); 
  }
}

//reads characters (either regular char or escape seq)
int editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) 
      die("retry read");
  }

  if (c == '\x1b') {
    char seq[3];
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
    //read into seq buffer for arrow key input (escape sequence)
    if (seq[0] == '[') {
      //page up/down keys
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
        if (seq[2] == '~') {
          switch (seq[1]) {
            case '1': return HOME_KEY;
            case '3': return DEL_KEY;
            case '4': return END_KEY;
            case '5': return PAGE_UP;
            case '6': return PAGE_DOWN;
            case '7': return HOME_KEY;
            case '8': return END_KEY;
          }
        }
      } 
      else {
      switch (seq[1]) {
        case 'A': return ARROW_UP;
        case 'B': return ARROW_DOWN;
        case 'C': return ARROW_RIGHT;
        case 'D': return ARROW_LEFT;
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
      }
    }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
      }
    }
    return '\x1b';
  } 
  else {
    return c;
  }
}

//grabs current cursor position 
int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  //grab max rows/cols detected and parse into buffer
  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) 
    return -1;
  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
    if (buf[i] == 'R') break;
    i++;
  }

  //buffer read
  buf[i] = '\0';
  if (buf[0] != '\x1b' || buf[1] != '[') 
    return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) 
    return -1;

  return 0;
}

//grabs the size of terminal
//if ioctl fails, consult escape sequence query (position cursor at very end and grab position)
int getWindowSize(int *rows, int *cols) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) 
  {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) 
      return -1;
    return getCursorPosition(rows, cols);
  } 
  else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

int editorRowCursor_xToRx(erow *row, int cx) {
  int rx = 0;
  int j;
  for (j = 0; j < cx; j++) {
    if (row->chars[j] == '\t')
      rx += (EDITOR_TAB_STOP - 1) - (rx % EDITOR_TAB_STOP);
    rx++;
  }
  return rx;
}

//grab chars string on an erow to fill render string (deals with tab spacings)
void editorUpdateRow(erow *row) {
  int tabs = 0;
  int j;
  for (j = 0; j < row->size; j++)
    if (row->chars[j] == '\t') tabs++;
  free(row->render);
  row->render = malloc(row->size + tabs*(EDITOR_TAB_STOP - 1) + 1);

  int idx = 0;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while (idx % EDITOR_TAB_STOP != 0) 
        row->render[idx++] = ' ';
    } else {
      row->render[idx++] = row->chars[j];
    }
  }
  row->render[idx] = '\0';
  row->rsize = idx;
}



//allocate space for row and copy string over 
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
  E.dirty++;
}

//free memory owned by specific erow
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

//inserts character into erow with given position
void editorRowInsertChar(erow *row, int at, int c) {
  if (at < 0 || at > row->size) 
    at = row->size;
  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  editorUpdateRow(row);
  E.dirty++;
}

//appending a string to a row
void editorRowAppendString(erow *row, char *s, size_t len) {
  row->chars = realloc(row->chars, row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\0';
  editorUpdateRow(row);
  E.dirty++;
}

//delete character in erow (overwrite deleted char with char after it)
void editorRowDelChar(erow *row, int at) {
  if (at < 0 || at >= row->size) return;
  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
  row->size--;
  editorUpdateRow(row);
  E.dirty++;
}

//del leftside character of cursor
void editorDelChar() {
  //cursor past EOF
  if (E.cursor_y == E.numrows) return;
  if (E.cursor_x == 0 && E.cursor_y == 0) return;
  erow *row = &E.row[E.cursor_y];

  if (E.cursor_x > 0) {
    editorRowDelChar(row, E.cursor_x - 1);
    E.cursor_x--;
  } 
  //deleting a line so move all current contents to above line
  else {
    E.cursor_x = E.row[E.cursor_y - 1].size;
    editorRowAppendString(&E.row[E.cursor_y - 1], row->chars, row->size);
    editorDelRow(E.cursor_y);
    E.cursor_y--;
  }
}

/*** editor operations ***/
//appends a new row before character insertion
void editorInsertChar(int c) {
  if (E.cursor_y == E.numrows) {
    editorInsertRow(E.numrows, "", 0);
  }
  editorRowInsertChar(&E.row[E.cursor_y], E.cursor_x, c);
  E.cursor_x++;
}

void editorInsertNewline() {
  //if beginning of line, insert new blank row above line
  if (E.cursor_x == 0) {
    editorInsertRow(E.cursor_y, "", 0);
  } 
  //split line into 2 rows
  else {
    erow *row = &E.row[E.cursor_y];
    editorInsertRow(E.cursor_y + 1, &row->chars[E.cursor_x], row->size - E.cursor_x);
    row = &E.row[E.cursor_y];
    row->size = E.cursor_x;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
  }
  E.cursor_y++;
  E.cursor_x = 0;
}


/*** file i/o  ***/
//convert contents into buffer for saving 
char *editorRowsToString(int *buflen) {
  int totlen = 0;
  int j;
  for (j = 0; j < E.numrows; j++)
    totlen += E.row[j].size + 1;
  *buflen = totlen;
  char *buf = malloc(totlen);
  char *p = buf;
  for (j = 0; j < E.numrows; j++) {
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    *p = '\n';
    p++;
  }
  return buf;
}


//read from file if possible and output each line to editor
void editorOpen(char *filename) {
  free(E.filename);
  E.filename = strdup(filename);

  FILE *fp = fopen(filename, "r");
  if (!fp) die("fopen");

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 && (line[linelen - 1] == '\n' ||
                           line[linelen - 1] == '\r'))
      linelen--;
    editorInsertRow(E.numrows, line, linelen);
  }
  free(line); 
  fclose(fp); 
  //reset dirty flag
  E.dirty = 0;
}

void editorSave() {
  if (E.filename == NULL) {
    E.filename = editorPrompt("Save as: %s (ESC to cancel)");
    if (E.filename == NULL) {
      editorSetStatusMessage("Save aborted");
      return;
    }
  }

  int len;
  char *buf = editorRowsToString(&len);
  //create new file if not existing,open for read/writes
  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
  if (fd != -1) {
    if (ftruncate(fd, len) != -1) {
      if (write(fd, buf, len) == len) {
        //set file to specified length
        ftruncate(fd, len);
        write(fd, buf, len);
        close(fd);
        free(buf);
        //changes have been saved 
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

/*** append buffer ***/
struct abuf {
  char *b;
  int len;
};


//set empty buffer
#define ABUF_INIT {NULL, 0}


//continuously append strings to buffer so writes are not scattered
void abAppend(struct abuf *ab, const char *s, int len) {
  //allocate for curr string + appended component
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL) 
    return;
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

//dealloc memory used by buffer
void abFree(struct abuf *ab) {
  free(ab->b);
}

//displays prompt and allows for user input 
char *editorPrompt(char *prompt) {
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
    } 
    else if (c == '\x1b') {
      editorSetStatusMessage("");
      free(buf);
      return NULL;
    } 
    
    else if (c == '\r') {
      if (buflen != 0) {
        editorSetStatusMessage("");
        return buf;
      }
    } 
    
    else if (!iscntrl(c) && c < 128) {
      if (buflen == bufsize - 1) {
        bufsize *= 2;
        buf = realloc(buf, bufsize);
      }
      buf[buflen++] = c;
      buf[buflen] = '\0';
    }

  }

}

//wasd movement to move cursor
void editorMoveCursor(int key) {
  //prevent user from scrolling past current line end
  erow *row = (E.cursor_y >= E.numrows) ? NULL : &E.row[E.cursor_y];
  switch (key) {
    case ARROW_LEFT:
      if(E.cursor_x != 0) {
        E.cursor_x--;
      }
      //move to end of previous line 
      else if (E.cursor_y > 0) {
        E.cursor_y--;
        E.cursor_x = E.row[E.cursor_y].size;
      }
      break;
    case ARROW_RIGHT:
      if (row && E.cursor_x < row->size) {
        E.cursor_x++;
      }
      //move to start of next line
      else if (row && E.cursor_x == row->size) {
        E.cursor_y++;
        E.cursor_x = 0;
      }
      break;
    case ARROW_UP:
      if(E.cursor_y != 0) {
        E.cursor_y--;
      }
      break;
    case ARROW_DOWN:
      if (E.cursor_y < E.numrows) {
        E.cursor_y++;
      } 
      break;
  }

  row = (E.cursor_y >= E.numrows) ? NULL : &E.row[E.cursor_y];
  int rowlen = row ? row->size : 0;
  if (E.cursor_x > rowlen) {
    E.cursor_x = rowlen;
  }
}

//handles processing for read in character
void editorProcessKeypress() {
  static int quit_times = EDITOR_QUIT_TIMES;

  int c = editorReadKey();
  //map quit key to ctrl+Q
  switch (c) {
    case '\r': 
      editorInsertNewline();
      break;

    case CTRL_KEY('q'):
    if (E.dirty && quit_times > 0) {
        editorSetStatusMessage("WARNING!!! File has unsaved changes. "
          "Press Ctrl-Q %d more times to quit.", quit_times);
        quit_times--;
        return;
      }
    //clear screen
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;

    case CTRL_KEY('s'):
      editorSave();
      break;

    case HOME_KEY:
      E.cursor_x = 0;
      break;
    case END_KEY:
      if (E.cursor_y < E.numrows)
        E.cursor_x = E.row[E.cursor_y].size;
      break;
    
    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
      if (c == DEL_KEY) 
        editorMoveCursor(ARROW_RIGHT);
      editorDelChar();
      break;
    
    //scroll up or down a page using PAGE_UP PAGE_DOWN keys
    case PAGE_UP:
    case PAGE_DOWN:
      {
        if (c == PAGE_UP) {
          E.cursor_y = E.rowoffset;
        } else if (c == PAGE_DOWN) {
          E.cursor_y = E.rowoffset + E.screen_rows - 1;
          if (E.cursor_y > E.numrows) E.cursor_y = E.numrows;
        }

        int times = E.screen_rows;
        while (times--)
          editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      }
      break;
    
    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editorMoveCursor(c);
      break;
    
    case CTRL_KEY('l'):
    case '\x1b':
      break;
    
    default: 
      editorInsertChar(c);
      break;
  }

  quit_times = EDITOR_QUIT_TIMES;
}

//keep cursor within window when user scrolls 
void editorScroll() {
  E.rx = 0; 
  if (E.cursor_y < E.numrows) {
    E.rx = editorRowCursor_xToRx(&E.row[E.cursor_y], E.cursor_x);
  }
  if (E.cursor_y < E.rowoffset) {
    E.rowoffset = E.cursor_y;
  }
  if (E.cursor_y >= E.rowoffset + E.screen_rows) {
    E.rowoffset = E.cursor_y - E.screen_rows + 1;
  }
  if (E.rx < E.coloffset) {
    E.coloffset = E.rx;
  }
  if (E.rx >= E.coloffset + E.screen_cols) {
    E.coloffset = E.rx - E.screen_cols + 1;
  }
}

//mark all rows with ~
void editorMarkRows(struct abuf *ab) {
  int r;
  for (r = 0; r < E.screen_rows; r++) {
    int filerow = r + E.rowoffset; 
    //check to see if we are drawing row that's part of the text buffer or row after text buffer end
    if(filerow >= E.numrows) {
      //only have welcome message show up if file read in is empty
    if (E.numrows == 0 && r == E.screen_rows / 3) {
      char welcome[80];
      int welcomelen = snprintf(welcome, sizeof(welcome),
        "Lite Editor -- version %s", EDITOR_VERSION);
      if (welcomelen > E.screen_cols) welcomelen = E.screen_cols;
      //centering welcome header 
      int padding = (E.screen_cols - welcomelen) / 2;
      if (padding) {
        abAppend(ab, "~", 1);
        padding--;
      }
      while (padding--) 
        abAppend(ab, " ", 1);
      abAppend(ab, welcome, welcomelen);
    } 
    else {
      abAppend(ab, "~", 1);
    }
    }
    else{
      int len = E.row[filerow].rsize - E.coloffset;
      if (len < 0) len = 0;
      if(len > E.screen_cols) 
        len = E.screen_cols; 
      abAppend(ab, &E.row[filerow].render[E.coloffset], len);
    }

    //erase line to right of cursor
    abAppend(ab, "\x1b[K", 3);

    abAppend(ab, "\r\n", 2);
    }
}


void editorDrawStatusBar(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 4);
  //printing out name of file
  char status[80];
  //have file line count align to right screen end
  char rstatus[80];
  //show if editor has unsaved changes or not 
  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
    E.filename ? E.filename : "[No Name]", E.numrows,
    E.dirty ? "(modified)" : "");
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",E.cursor_y + 1, E.numrows);
  if (len > E.screen_cols) len = E.screen_cols;
  abAppend(ab, status, len);
  while (len < E.screen_cols) {
    if (E.screen_cols - len == rlen) {
      abAppend(ab, rstatus, rlen);
      break;
    } 
    else {
    abAppend(ab, " ", 1);
    len++;
  }
  }
  abAppend(ab, "\x1b[m", 3);
  abAppend(ab, "\r\n", 2);
}

//show status message for 5 seconds 
void editorDrawMessageBar(struct abuf *ab) {
  abAppend(ab, "\x1b[K", 3);
  int msglen = strlen(E.statusmsg);
  if (msglen > E.screen_cols) msglen = E.screen_cols;
  if (msglen && time(NULL) - E.statusmsg_time < 5)
    abAppend(ab, E.statusmsg, msglen);
}

//VT100 escape sequence https://vt100.net/docs/vt100-ug/chapter3.html 

void editorRefreshScreen() {
  editorScroll();

  struct abuf ab = ABUF_INIT;
  //hide cursor during screen refresh
  abAppend(&ab, "\x1b[?25l", 6);
  //escape sequence + H = positions cursor at first row and column of cleared terminal 
  abAppend(&ab, "\x1b[H", 3);

  editorMarkRows(&ab); 
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);

  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cursor_y - E.rowoffset) + 1, (E.rx - E.coloffset) + 1);
  abAppend(&ab, buf, strlen(buf));

  //unhide cursor
  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

//sets the status message 
void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}


/****init  ******/
void initEditor() {
  E.cursor_x = 0; 
  E.cursor_y = 0; 
  E.rx = 0; 
  E.rowoffset = 0; 
  E.coloffset = 0;
  E.numrows = 0; 
  E.row = NULL; 
  E.dirty = 0;
  E.filename = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;
  if (getWindowSize(&E.screen_rows, &E.screen_cols) == -1) 
    die("getWindowSize error");
  //dont draw line at bottom of screen (leave space for status bar and status message)
  E.screen_rows -= 2;
}


int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor(); 

  if (argc >= 2) {
    editorOpen(argv[1]);
  }

  //initialize a status message that shows up for 5 seconds or until first trigger of user input 
  editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit");

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }



  return 0;
}