
PREFIX=/usr/local
BINDIR=$(PREFIX)/bin
LIBDIR=$(PREFIX)/lib
LIBEXECDIR=$(PREFIX)/libexec
CONFIGDIR=$$(getent passwd $(SUDO_USER) | cut -d: -f6)/.config

CC=gcc
CFLAGS=-Wall -g

