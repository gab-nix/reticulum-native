#include "reticulum/identity.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void usage(FILE *stream) {
    fprintf(stream,
            "Usage:\n"
            "  rnid generate IDENTITY_FILE\n"
            "  rnid show IDENTITY_FILE\n");
}

static void print_hex(const uint8_t *bytes, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        printf("%02x", bytes[i]);
    }
}

static int write_identity(const char *path, const rns_identity *identity) {
    uint8_t private_key[RNS_IDENTITY_PRIVATE_SIZE];
    FILE *file;
    mode_t previous_mask;

    if (!rns_identity_export_private(identity, private_key)) {
        fprintf(stderr, "rnid: identity has no private key\n");
        return 0;
    }
    previous_mask = umask(0077);
    file = fopen(path, "wbx");
    umask(previous_mask);
    if (file == NULL) {
        fprintf(stderr, "rnid: cannot create %s: %s\n", path, strerror(errno));
        return 0;
    }
    if (fwrite(private_key, 1, sizeof(private_key), file) != sizeof(private_key) ||
        fflush(file) != 0 || fclose(file) != 0) {
        fprintf(stderr, "rnid: cannot write %s\n", path);
        remove(path);
        return 0;
    }
    return 1;
}

static int read_identity(const char *path, rns_identity *identity) {
    uint8_t private_key[RNS_IDENTITY_PRIVATE_SIZE];
    FILE *file = fopen(path, "rb");
    int trailing;

    if (file == NULL) {
        fprintf(stderr, "rnid: cannot open %s: %s\n", path, strerror(errno));
        return 0;
    }
    if (fread(private_key, 1, sizeof(private_key), file) != sizeof(private_key)) {
        fprintf(stderr, "rnid: %s is not a 64-byte Reticulum private identity\n", path);
        fclose(file);
        return 0;
    }
    trailing = fgetc(file);
    if (trailing != EOF || fclose(file) != 0) {
        fprintf(stderr, "rnid: %s has trailing data or could not be closed\n", path);
        return 0;
    }
    if (!rns_identity_from_private(identity, private_key)) {
        fprintf(stderr, "rnid: invalid identity data in %s\n", path);
        return 0;
    }
    return 1;
}

static void show_identity(const rns_identity *identity) {
    uint8_t public_key[RNS_IDENTITY_PUBLIC_SIZE];
    rns_identity_export_public(identity, public_key);
    printf("Identity : ");
    print_hex(identity->hash, RNS_TRUNCATED_HASH_SIZE);
    printf("\nPublic   : ");
    print_hex(public_key, sizeof(public_key));
    printf("\n");
}

int main(int argc, char **argv) {
    rns_identity identity;

    if (argc != 3) {
        usage(stderr);
        return EXIT_FAILURE;
    }
    if (strcmp(argv[1], "generate") == 0) {
        if (!rns_identity_generate(&identity) || !write_identity(argv[2], &identity)) {
            return EXIT_FAILURE;
        }
        show_identity(&identity);
        return EXIT_SUCCESS;
    }
    if (strcmp(argv[1], "show") == 0) {
        if (!read_identity(argv[2], &identity)) {
            return EXIT_FAILURE;
        }
        show_identity(&identity);
        return EXIT_SUCCESS;
    }
    usage(stderr);
    return EXIT_FAILURE;
}
