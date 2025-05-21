TARGET      = kxo
kxo-objs    = main.o game.o xoroshiro.o mcts.o negamax.o zobrist.o
obj-m      := $(TARGET).o

ccflags-y   := -std=gnu99 -Wno-declaration-after-statement
KDIR       ?= /lib/modules/$(shell uname -r)/build
PWD         := $(shell pwd)


USER_SRC  := xo-user.c \
             user_game.c user_mcts.c user_negamax.c \
             user_xoroshiro.c user_zobrist.c
USER_OBJ  := $(USER_SRC:.c=.o)

USER_CFLAGS := -std=gnu99 -Wno-declaration-after-statement -Wall -g
USER_LDFLAGS := 

GIT_HOOKS := .git/hooks/applied
all: kmod xo-user


kmod: $(GIT_HOOKS) main.c
	$(MAKE) -C $(KDIR) M=$(PWD) modules

xo-user: $(USER_OBJ)
	$(CC) $(USER_CFLAGS) $(USER_LDFLAGS) -o $@ $^


%.o: %.c
	$(CC) $(USER_CFLAGS) -c $< -o $@


$(GIT_HOOKS):
	@scripts/install-git-hooks
	@echo


clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(RM) xo-user $(USER_OBJ)
