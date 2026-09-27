/* fetch command backend (see fetch.c). */
#ifndef FETCH_H
#define FETCH_H

/* print logo + live system info through emit (usually sh_print,
 * so shell redirection keeps working) */
void fetch_run(void (*emit)(const char *s));

#endif
