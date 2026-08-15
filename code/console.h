#ifndef CONSOLE_H
#define CONSOLE_H

#include <termios.h>
#include <unistd.h>
#include <signal.h>

static struct termios orig_termios;

void handle_sigint(int sig);
void enable_raw_mode(void);
void disable_raw_mode(void);

#endif