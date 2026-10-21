// impulse: malloc overlap / corruption probe (v2, no %p, no UB).
//
// musl (ckb fork) routes allocations above MMAP_THRESHOLD to mmap; the
// OpenVM syscall hijack FAILS all mmaps by design. Question: do big
// mallocs return sane, non-overlapping chunks on the VM?

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

extern char _end[];
static unsigned long addr(void* p) { return (unsigned long)(size_t)p; }

int main()
{
	printf("_end=%lx sbrk0=%lx stackvar=%lx\n", (unsigned long)_end, (unsigned long)sbrk(0), addr((void*)&_end));

	void* m1 = mmap(0, 4096, 3 /*RW*/, 0x22 /*PRIVATE|ANON*/, -1, 0);
	printf("mmap4k=%lx failed=%d\n", addr(m1), m1 == (void*)-1);

	void* m2 = mmap(0, 10 * 1024 * 1024, 3, 0x22, -1, 0);
	printf("mmap10M=%lx failed=%d\n", addr(m2), m2 == (void*)-1);

	void* h1 = malloc(10 * 1024 * 1024);
	printf("huge1=%lx nonzero=%d\n", addr(h1), h1 != nullptr);
	((unsigned char*)h1)[0] = 0x5A;
	((unsigned char*)h1)[10 * 1024 * 1024 - 1] = 0x5A;
	printf("huge1 writable: first=%02x last=%02x\n", ((unsigned char*)h1)[0], ((unsigned char*)h1)[10 * 1024 * 1024 - 1]);

	{
		long stack_probe = 0;
		printf("stackaddr=%lx\n", (unsigned long)(size_t)&stack_probe);
		void* b1 = sbrk(4096);
		printf("sbrk4k=%lx\n", addr(b1));
		void* b2 = sbrk(10 * 1024 * 1024);
		printf("sbrk10M=%lx\n", addr(b2));
		void* m3 = malloc(140 * 1024);
		printf("malloc140K=%lx\n", addr(m3));
	}
	printf("overlap probe done\n");
	return 0;
}
