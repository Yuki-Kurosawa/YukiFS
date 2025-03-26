// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <ctype.h> // For tolower()
#include <getopt.h> // For getopt_long()
#include <sys/types.h> // For getuid()
#include <unistd.h>    // For getuid()

#include "../../include/version.h"
#include "../../include/file_table.h"
#include "../../tools/built-in.h"

void print_usage(const char *program_name) {
    printf("Usage: %s [OPTIONS] <device_or_image_path>\n", program_name);
    printf("Options:\n");
    printf("  -h, --help     Show this help message\n");
    printf("  -v, --version  Show version information\n");
}

int main(int argc, char *argv[])
{
    char *device_path = NULL;

    static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'v'},
        {0, 0, 0, 0}
    };

    int opt;
    int option_index = 0;

    while ((opt = getopt_long(argc, argv, "hv", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'h':
                print_usage(argv[0]);
                return 0;
            case 'v':
                printf("infofs version %s\n", INFOFS_VERSION_STRING);
                return 0;
            case '?':
                print_usage(argv[0]);
                return 1;
            default:
                fprintf(stderr, "Error: Unknown option.\n");
                print_usage(argv[0]);
                return 1;
        }
    }

    if (optind == argc - 1) {
        device_path = argv[optind];
    } else {
        fprintf(stderr, "Error: You must specify a device or image path.\n");
        print_usage(argv[0]);
        return 1;
    }

    // Check if the device path exists
    struct stat path_stat;
    if (stat(device_path, &path_stat) == -1) {
        fprintf(stderr, "Error: Cannot access '%s': %s\n", device_path, strerror(errno));
        return 1;
    }

    // TODO: Implement the actual functionality to display filesystem information
    printf("Displaying information for YukiFS on %s\n", device_path);
    
    return 0;
}
