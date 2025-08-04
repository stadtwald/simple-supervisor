# simple supervisor

## Quickstart

This program is configured at compile time.

1. If you are including this in a larger codebase, it is recommend you copy this source directly into your source tree.
2. Edit `config.h` to customise.
3. Compile using your favourite C compile: `cc simple-supervisor.c -o simple-supervisor`
4. Simply run the resulting binary in whatever context you want the process tree to spawn.

## Support

### Operating systems

The goal is to support all modern Linux and BSD environments.  "Bare metal" and Docker are reasonable places to run this program.

The developer has tested on Debian Linux and OpenBSD.  Feel free to send patches for other UNIX-like operating systems if necessary.

### C compilers and libraries

GCC and LLVM/Clang should compile this program without errors and warnings, under standard configuration.  Support for other compilers is unknown.

Only a standard C library with normal UNIX system headers are required.

