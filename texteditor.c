#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <ctype.h> 
#include <stdio.h> 
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>


#define CTRL_KEY(k) ((k) & 0x1f)
#define EDITOR_VERSION "1.0"

//home_key = start of line, end_key = end of line
enum editorKey {
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

//raw mode is neccessary so text editor can continue accepting input 
//terminal attributes read into a termios struct
struct editorConfig{
  int cursor_x; 
  int cursor_y; 
  int screen_rows;
  int screen_cols;
  struct termios original_term;
};

struct editorConfig E;

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

//wasd movement to move cursor
void editorMoveCursor(int key) {
  switch (key) {
    case ARROW_LEFT:
      if(E.cursor_x != 0) {
        E.cursor_x--;
      }
      break;
    case ARROW_RIGHT:
      if (E.cursor_x != E.screen_cols - 1) {
        E.cursor_x++;
      }
      break;
    case ARROW_UP:
      if(E.cursor_y != 0) {
        E.cursor_y--;
      }
      break;
    case ARROW_DOWN:
      if (E.cursor_y != E.screen_rows - 1) {
        E.cursor_y++;
      } 
      break;
  }
}

//handles processing for read in character
void editorProcessKeypress() {
  int c = editorReadKey();
  //map quit key to ctrl+Q
  switch (c) {
    case CTRL_KEY('q'):
    //clear screen
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;

    case HOME_KEY:
      E.cursor_x = 0;
      break;
    case END_KEY:
      E.cursor_x = E.screen_cols - 1;
      break;
    
    case PAGE_UP:
    case PAGE_DOWN:
      {
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
  }
}

//mark all rows with ~
void editorMarkRows(struct abuf *ab) {
  int r;
  for (r = 0; r < E.screen_rows; r++) {
    if (r == E.screen_rows / 3) {
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
    //erase line to right of cursor
    abAppend(ab, "\x1b[K", 3);

    if (r < E.screen_rows - 1) {
      abAppend(ab, "\r\n", 2);
    }
  }
}

//VT100 escape sequence https://vt100.net/docs/vt100-ug/chapter3.html 

void editorRefreshScreen() {
  struct abuf ab = ABUF_INIT;
  //hide cursor during screen refresh
  abAppend(&ab, "\x1b[?25l", 6);
  //escape sequence + H = positions cursor at first row and column of cleared terminal 
  abAppend(&ab, "\x1b[H", 3);

  editorMarkRows(&ab); 

  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cursor_y + 1, E.cursor_x + 1);
  abAppend(&ab, buf, strlen(buf));

  //unhide cursor
  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}


/****init  ******/
void initEditor() {
  E.cursor_x = 0; 
  E.cursor_y = 0; 
  if (getWindowSize(&E.screen_rows, &E.screen_cols) == -1) 
    die("getWindowSize error");
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