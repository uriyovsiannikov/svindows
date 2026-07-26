/*
 * user/hello.c - a plain, portable C program.
 *
 * No Windows or NTOS specifics: it just uses the standard library and a normal
 * int main(). It is compiled and linked the ordinary way (entry defaults to the
 * CRT's mainCRTStartup) and runs on NTOS unchanged -- the point being that a
 * standard program needs no special glue to run here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    printf("Hello from a standard C program on NTOS!\n");
    printf("  argc=%d, argv[0]=%s\n", argc, argc ? argv[0] : "(none)");

    int *squares = (int *)malloc(8 * sizeof(int));
    if (squares) {
        for (int i = 0; i < 8; i++)
            squares[i] = i * i;
        printf("  squares:");
        for (int i = 0; i < 8; i++)
            printf(" %d", squares[i]);
        printf("\n");
        free(squares);
    }

    char buf[32];
    strcpy(buf, "portable C");
    printf("  strcpy -> \"%s\" (len %d)\n", buf, (int)strlen(buf));

    return 0;
}
