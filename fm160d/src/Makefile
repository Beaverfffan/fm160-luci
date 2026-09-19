# SPDX-License-Identifier: GPL-2.0-or-later
#
# Plain Makefile used by the OpenWrt package (see ../Makefile, Build/Compile).
# Kept deliberately dependency-free: no autotools, no configure step.

CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall
LDFLAGS ?=

BIN      = fm160d
OBJS     = atq.o cmds.o main.o pdu.o sched.o sms.o state.o ubus_methods.o
LIBS     = -lubus -lubox -lblobmsg_json

all: $(BIN)

# Link order matters: objects must precede $(LDFLAGS)/$(LIBS) so that
# -Wl,--as-needed (used by the OpenWrt toolchain) does not drop the libraries
# before it has seen the references to them.
$(BIN): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS) $(LIBS)

%.o: %.c fm160d.h
	$(CC) $(CFLAGS) -c -o $@ $<

# pdu.c is the one source that deliberately does not include fm160d.h - it is
# pure transformation with no modem and no ubus, which is what lets the
# host-side test compile it on its own (23-sms-pdu-test.sh).
pdu.o: pdu.c pdu.h

clean:
	rm -f $(OBJS) $(BIN)

.PHONY: all clean
