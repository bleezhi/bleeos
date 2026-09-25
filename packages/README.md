# BleeOS user repository

Community `.blee` packages live here. Each package is a directory:

```
packages/mypkg/
  PKGINFO        # name=<...>, version=<...>
  hello.sh       # files, installed under /pkg/mypkg/
  README
```

## Rules (enforced by CI)

- `name=`: lowercase `a-z 0-9 _ -`, max 32 chars (this is also what
  the in-OS installer accepts — anything else is refused).
- `version=`: max 16 chars. Bump it on updates.
- Paths: relative, max 64 chars, no `.`/`..` tricks, no absolute paths.
- Sizes: archive ≤ 8192 bytes, each file ≤ 768 bytes, ≤ 32 files.
- Content: scripts + data + docs only (BleeOS has no program loader,
  so no binaries). Keep it family-friendly and yours to share.

## Publish yours

1. Fork the repo, add `packages/<name>/` (dir + PKGINFO + files).
2. Run `make pkgs` — it rebuilds `packages/<name>.blee` and copies it
   with the others into `docs/` (the website downloads).
3. Add your entry to `docs/packages.json` (`name`, `version`,
   `files`, `size`, `desc`, `file`, `by` = your handle).
4. Open a pull request. CI rebuilds every package, rejects rule
   violations, and rejects a `packages.json` that disagrees with the
   built `.blee` files. Merge = moderation: only what passes lands.

## Try one

```
dd if=hello.blee of=hdd.img bs=512 seek=2048 conv=notrunc
```

Attach the disk in QEMU, then in BleeOS:

```
pkg install-hd 2048
pkg info hello
run /pkg/hello/hello.sh
```
