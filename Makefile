brain: brain.c
	$(CC) -D_GNU_SOURCE -std=c99 $< -o $@ -lxcb -lxcb-shm

clean:
	$(RM) brain
