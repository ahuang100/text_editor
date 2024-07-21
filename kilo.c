#include <termios.h>
#include <unistd.h>

//raw mode is neccessary so text editor can continut accepting input 
void enableRawMode() {
  struct termios raw;
  tcgetattr(STDIN_FILENO, &raw);
  //turning off ECHO to prevent inputted char from appearing on terminal 
  raw.c_lflag &= ~(ECHO);
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}
int main() {
  enableRawMode();
  char c;
  while (read(STDIN_FILENO, &c, 1) == 1 && c != 'q');
  return 0;
}