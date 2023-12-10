NAME = mount.wfs mkfs.wfs fsck.wfs

CC = gcc
CFLAGS = -Wall -Werror -pedantic -std=gnu18
FUSE_CFLAGS = `pkg-config fuse --cflags --libs`

.PHONY: all
all: $(NAME)

.PHONY: mount.wfs
mount.wfs:
	$(CC) $(CFLAGS) mount.wfs.c $(FUSE_CFLAGS) -o mount.wfs types.h

.PHONY: mkfs.wfs
mkfs.wfs:
	$(CC) $(CFLAGS) -o mkfs.wfs mkfs.wfs.c types.h

.PHONY: fsck.wfs
fsck.wfs:
	$(CC) $(CFLAGS) -o fsck.wfs fsck.wfs.c types.h

.PHONY: clean
clean:
	rm -rf $(NAME)
