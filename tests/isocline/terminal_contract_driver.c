/*
  terminal_contract_driver.c

  This file is part of cjsh, CJ's Shell

  MIT License

  Copyright (c) 2026 Caden Finley

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include "common.h"

#include "term.h"
#include "tty.h"

static volatile sig_atomic_t handled;
static volatile sig_atomic_t mask_ok;

static void catch_signal(int signum) {
    sigset_t mask;
    (void)sigprocmask(SIG_SETMASK, NULL, &mask);
    mask_ok = sigismember(&mask, SIGUSR1);
    handled = signum;
    (void)write(STDERR_FILENO, "HANDLED\n", 8);
}

static void catch_siginfo(int signum, siginfo_t* info, void* context) {
    (void)context;
    if (info != NULL && info->si_signo == signum) {
        catch_signal(signum);
    }
}

static bool matches_osc(const char* response, void* arg) {
    (void)arg;
    return strcmp(response, "4;0;rgb:ff/ff/ff") == 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        return 2;
    }
    const char* scenario = argv[1];
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGUSR1);
    action.sa_handler = SIG_DFL;
    if (strcmp(scenario, "ignore") == 0) {
        action.sa_handler = SIG_IGN;
    } else if (strcmp(scenario, "custom") == 0 || strcmp(scenario, "reset") == 0) {
        action.sa_handler = catch_signal;
        if (strcmp(scenario, "reset") == 0) {
            action.sa_flags = SA_RESETHAND;
        }
    } else if (strcmp(scenario, "siginfo") == 0) {
        action.sa_sigaction = catch_siginfo;
        action.sa_flags = SA_SIGINFO;
    }
    const int signals[] = {SIGINT, SIGTERM, SIGHUP};
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) {
        (void)sigaction(signals[i], &action, NULL);
    }
    if (strcmp(scenario, "reset") == 0) {
        struct sigaction observed;
        (void)sigaction(SIGINT, NULL, &observed);
        // Darwin applies SA_RESETHAND but omits it from the returned action,
        // so libraries cannot discover it when wrapping an existing handler.
        if ((observed.sa_flags & SA_RESETHAND) == 0) {
            (void)printf("UNREPORTED_RESETHAND\nSIGNAL_READY\n");
            (void)fflush(stdout);
            // Wait for the peer to consume the skip marker before closing the
            // PTY: Darwin can discard queued output when the last slave closes.
            (void)getchar();
            return 77;
        }
    }

    alloc_t memory = {malloc, realloc, free};
    tty_t* tty = tty_new(&memory, STDIN_FILENO);
    if (tty == NULL) {
        return 3;
    }
    // Allow the Python PTY peer to be scheduled between query and response on
    // busy CI runners. Incomplete escape sequences still time out promptly.
    tty_set_esc_delay(tty, 1000, 100);
    if (strcmp(scenario, "query") == 0 || strcmp(scenario, "osc") == 0) {
        term_t* term = term_new(&memory, tty, true, true, STDOUT_FILENO);
        if (term == NULL) {
            return 4;
        }
        (void)tty_start_raw(tty);
        (void)write(STDERR_FILENO, "QUERY_READY\n", 12);
        ssize_t row = 0, column = 0;
        bool matched;
        if (strcmp(scenario, "osc") == 0) {
            char response[128];
            (void)write(STDOUT_FILENO, "\x1b]4;0;?\x07", 8);
            matched = tty_read_esc_response(tty, ']', true, response, sizeof(response), matches_osc,
                                            NULL);
        } else {
            matched = term_query_cursor_pos(term, &row, &column);
        }
        (void)printf("QUERY:%d:%zd:%zd\n", matched, row, column);
        (void)fflush(stdout);
        long length = 0;
        if (argc > 2) {
            char* end = NULL;
            errno = 0;
            length = strtol(argv[2], &end, 10);
            if (errno != 0 || end == argv[2] || *end != '\0' || length < 0 || length > INT_MAX) {
                return 2;
            }
        }
        for (int i = 0; i < length; ++i) {
            uint8_t c = 0;
            if (!tty_readc_noblock(tty, &c, 1000)) {
                return 5;
            }
            (void)printf("%02x", (unsigned)c);
        }
        (void)printf("\nREPLAY_DONE\n");
        term_free(term);
    } else {
        (void)tty_start_raw(tty);
        (void)write(STDERR_FILENO, "SIGNAL_READY\n", 13);
        while (tty_read(tty) != KEY_ENTER) {
        }
        (void)printf("HANDLER:%d:%d\n", (int)handled, (int)mask_ok);
    }
    tty_free(tty);
    struct sigaction restored;
    (void)sigaction(SIGINT, NULL, &restored);
    const bool restore_ok =
        strcmp(scenario, "reset") == 0
            ? restored.sa_handler == SIG_DFL
            : ((restored.sa_flags & SA_SIGINFO) == (action.sa_flags & SA_SIGINFO) &&
               restored.sa_handler == action.sa_handler &&
               sigismember(&restored.sa_mask, SIGUSR1) == 1);
    (void)printf("RESTORED:%d\n", restore_ok);
    return restore_ok ? 0 : 6;
}
