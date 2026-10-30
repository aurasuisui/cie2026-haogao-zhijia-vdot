NAME = cie-hello
SRCS = cie-hello.c

# inject host facts at build time so the banner is reproducible
# HELLO_NAME 可覆盖：默认填团队标识（阶段一要求 hello xiangshan, I am xxx, IP address）
HELLO_NAME ?= xujiabao (haogao-zhijia)
UIP   := $(shell hostname -I 2>/dev/null | awk '{print $$1}')
CFLAGS += '-DHELLO_USER="$(HELLO_NAME)"' '-DHELLO_IP="$(UIP)"'

include $(AM_HOME)/Makefile.app
