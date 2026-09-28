/* BleeOS POSIX-style shell interface. */
#ifndef SHELL_H
#define SHELL_H

#include "drivers.h"

/* Filesystem constants */
#define FS_MAX 96
#define FS_DATA 768

/* Filesystem node type (ramfs) */
typedef struct { u8 used, is_dir; char name[24]; u8 parent; u16 size; char data[FS_DATA]; } fsnode_t;

extern fsnode_t fs[FS_MAX];  /* ramfs nodes */
extern char cwd[64];         /* current working directory */

void shell_run(u32 boot_sec, int verbose);
const char *shell_hostname(void);
const char *shell_cwd(void);     /* current working directory */
int shell_readline(char *buf);   /* line editor; len, -1 EOF, -2 cancel */
int shell_readpass(char *buf, u32 cap);  /* masked entry; len, -1 cancel */
int shell_uid(void);             /* current login uid */
const char *shell_user(void);    /* current login name */
int shell_exec(char *line);      /* run one command line (for terminal) */
void sh_set_term(int on);        /* terminal window owns I/O */
int sh_in_term(void);
int shell_fread(const char *path, char *buf, u32 cap);  /* bytes, -1 */
int shell_fwrite(const char *path, const char *data, u32 len);  /* 0 ok */
int shell_mkdir(const char *path);   /* 0 ok (exists ok) */
int shell_rm(const char *path);      /* file/empty dir; 0 ok */

#endif
