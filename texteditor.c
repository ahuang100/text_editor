#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <ctype.h> 
#include <stdio.h> 
#include <errno.h>



//raw mode is neccessary so text editor can continue accepting input 
//terminal attributes read into a termios struct
struct termios original_term;


//terminal functions

//force exit on program upon error
void die(const char *s) {
  perror(s);
  exit(1);
}

void disableRawMode() {
  if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_term) == -1)
    die("tcsetattr failed");
}

void enableRawMode() {
  if(tcgetattr(STDIN_FILENO, &original_term) == -1)
  {
    die("tcgetattr failed"); 
  }
  atexit(disableRawMode);
  struct termios raw = original_term;
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


int main() {
  enableRawMode();
  //print out the characters inputted into the terminal 
  //iscntrl checks if the character is printable first to the console 
  while (1) {
    char c = '\0';
    if(read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) 
      die("read failed"); 
    if (iscntrl(c)) {
      printf("%d\r\n", c);
    } else {
      printf("%d ('%c')\r\n", c, c);
    }
    if (c == 'q') break;
  }


  return 0;
}