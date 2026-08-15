#include "console.h"

void handle_sigint(int sig){
    disable_raw_mode();
    _exit(1);
}

void enable_raw_mode(void) {
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

void disable_raw_mode(void){
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}