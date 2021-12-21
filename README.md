scaf - simple auto cpu frequencer
=================================
sacf it's a simple program that monitors the cpu usage, system load and the
temperature over 5 seconds, by default, and simply tweaks the governors and
turbo boost only when it needs to.

Usage
-----
Invoke with no arguments sacf will start to run and do simple changes.

    sacf [-bltTv] [-g governor]

For more  detail information read the man page.


Installation
------------
Edit config.mk to match your local setup (sacf is installed into
the /usr/local namespace by default).

Afterwards enter the following command to build and install sacf
(if necessary as root):

    make clean install


TODO
====
- freebsd and openbsd support
