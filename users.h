/* BleeOS users database: /etc/passwd (name:uid:gid) + /etc/shadow
 * (name:salt$hash). Passwords are salted/iterated FNV-1a hashes --
 * hobby-grade, not real password security. ramfs is volatile, so
 * users added at runtime vanish on reboot. */
#ifndef USERS_H
#define USERS_H

void users_init(void);   /* seed root (password "root") if DB missing */
int  users_auth(const char *name, const char *pass);  /* 1 ok, 0 fail */
int  users_add(const char *name, const char *pass);   /* 0 ok, -1 error */
int  users_del(const char *name);       /* 0 ok, -1 (root/missing) */
int  users_setpass(const char *name, const char *pass);  /* 0 ok, -1 */
int  users_uid(const char *name);       /* uid, -1 if unknown */
int  users_validname(const char *name); /* 1 if usable as a login name */
/* persistence: call at boot; when on and an ATA disk exists, the DB
 * is loaded from / saved to reserved HDD sectors (survives reboot) */
void users_set_installed(int on);
/* force ramfs DB to disk now (installer, live media); 0 ok, -1 no disk */
int users_flush(void);
/* adopt a valid on-disk DB if present (UEFI boot probe); 0 ok, -1 none */
int users_try_restore(void);

#endif
