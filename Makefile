NAME ?= bannerd
ROOTFSDIR ?= _install

OBJS = bmp.o fb.o main.o
CFLAGS += -DSRV_NAME=\"$(NAME)\"

.PHONY: all clean install

all: $(NAME)

$(NAME): $(OBJS)
	$(CC) $(CFLAGS) -o $(NAME) $(OBJS)
	
clean:
	rm -fr *.o *~ $(NAME)

install:
	install $(NAME) $(ROOTFSDIR)/bin

