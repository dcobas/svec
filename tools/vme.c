#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <libvmebus.h>

static char usage_string[] =
	"usage: %s [-oh?] [ -w word ] [-v vme_address]\n"
	"[-s skip_bytes ] [-d data_width] "
	"[-a address_modifier] [-n word_count]\n";

void usage(char *prog)
{
	fprintf(stderr, usage_string, prog);
	exit(1);
}
int main(int argc, char *argv[])
{

	struct vme_mapping map;
	struct vme_mapping *mapp = &map;
	volatile void *ptr;
	unsigned int vmebase, am, data_width;
	unsigned int offset, skip_bytes;
	int i, count;
	int c;
	int write, offsets_on;
	uint32_t word;

	/* vme defaults */
	count = 1;
	am = VME_A32_USER_DATA_SCT;
	data_width = VME_D32;

	write = 0;
	offsets_on = 1;
	skip_bytes = 0;
	while ((c = getopt(argc, argv, "ov:s:d:a:n:w:")) != -1) {
		switch (c) {
		case 'o':
			offsets_on = 0;
			break;
		case 's':
			skip_bytes = strtoul(optarg, NULL, 0);
			break;
		case 'v':
			vmebase = strtoul(optarg, NULL, 0);
			break;
		case 'd':
			data_width = strtoul(optarg, NULL, 0);
			break;
		case 'a':
			am = strtoul(optarg, NULL, 0);
			break;
		case 'n':
			count = strtoul(optarg, NULL, 0);
			break;
		case 'w':
			write = 1;
			word = strtoul(optarg, NULL, 0);
			break;
		case '?':
		case 'h':
			usage(argv[0]);
			break;
		default:
			break;
		}
	}

	memset(mapp, 0, sizeof(*mapp));

	mapp->am = 		am;
	mapp->data_width = 	data_width;
	mapp->sizel = 		0x80000;
	mapp->vme_addrl =	vmebase;

	if ((ptr = vme_map(mapp, 1)) == NULL) {
		printf("could not map at 0x%08x\n", vmebase);
		exit(1);
	}

	fprintf(stderr, "vme 0x%08x kernel 0x%p user 0x%p\n",
			vmebase, mapp->kernel_va, mapp->user_va);
	offset = skip_bytes;
	for (i = 0; i < count; i++, offset += 4) {
		volatile uint32_t *addr = ptr + offset;
		if (!write) {
			if (offsets_on)
				printf("%08x: ", vmebase + offset);
			printf("%08x\n", ntohl(*addr));
		} else
			*addr = htonl(word);
	}

	vme_unmap(mapp, 1);
	return 0;
}
