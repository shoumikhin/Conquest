#include <stdio.h>

#include "routines.h"

//------------------------------------------------------------------------------
int main(int argc, char *argv[])
{
    int sock;

    if (2 == argc)
        return serverSide(argv[1]);
    else if (3 == argc)
            return clientSide(argv[1], argv[2]);
        else fprintf(stderr, "\nUsage : conquest <port> [<host>]\n\n"
                             "Conquest - The simpliest galactic strategy\n\n");

    return 0;
}
//------------------------------------------------------------------------------
