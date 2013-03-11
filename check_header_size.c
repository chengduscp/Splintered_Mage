#include "stdio.h"

typedef unsigned int uint32_t;

#include "journal.h"

int main(int argc, char const *argv[])
{
	printf("Journal header size: %d\n", (sizeof(journal_header_t)));
	return 0;
}