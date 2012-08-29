NAME ?= bannerd
ROOTFSDIR ?= _install

OBJS = animation.o bmp.o commands.o fb.o main.o
CFLAGS += -DSRV_NAME=\"$(NAME)\"

.PHONY: all clean install

all: $(NAME)

$(NAME): $(OBJS)
	$(CC) $(LDFLAGS) -o $(NAME) $(OBJS)
	
clean:
	rm -fr *.o *~ $(NAME)

install:
	install $(NAME) $(ROOTFSDIR)/bin

