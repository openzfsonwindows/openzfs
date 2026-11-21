// SPDX-License-Identifier: CDDL-1.0
/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * https://opensource.org/license/CDDL-1.0.
 */

/*
 * Exercise lseek(SEEK_DATA) and lseek(SEEK_HOLE) as the libspl wrappers
 * implement them on Windows, on a file with a known layout:
 *
 *	[0, 128K)	data
 *	[128K, 8M)	hole
 *	[8M, 8M+128K)	data
 *	[8M+128K, 16M)	hole, to EOF
 *
 * Usage: seektest <path on a ZFS dataset>
 * The file is created, and removed on success.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>

#define	K	1024ULL
#define	M	(1024ULL * 1024ULL)
#define	ERR	((uint64_t)-1)

static int failures;

static void
expect(int fd, uint64_t off, int whence, uint64_t want, int want_errno)
{
	uint64_t got;

	errno = 0;
	got = (uint64_t)lseek(fd, off, whence);

	if (got == want && (want != ERR || errno == want_errno))
		return;

	(void) fprintf(stderr, "FAIL lseek(%llu, SEEK_%s) = %lld errno %d, "
	    "expected %lld errno %d\n", (unsigned long long)off,
	    whence == SEEK_DATA ? "DATA" : "HOLE", (long long)got, errno,
	    (long long)want, want_errno);
	failures++;
}

int
main(int argc, char **argv)
{
	static char buf[128 * 1024];
	int fd;

	if (argc != 2) {
		(void) fprintf(stderr, "usage: %s <file>\n", argv[0]);
		return (1);
	}

	fd = open(argv[1], O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		(void) fprintf(stderr, "open %s: errno %d\n", argv[1], errno);
		return (1);
	}

	(void) memset(buf, 0xAB, sizeof (buf));
	if (write(fd, buf, sizeof (buf)) != (ssize_t)sizeof (buf) ||
	    lseek(fd, 8 * M, SEEK_SET) != (off_t)(8 * M) ||
	    write(fd, buf, sizeof (buf)) != (ssize_t)sizeof (buf) ||
	    ftruncate(fd, 16 * M) != 0) {
		(void) fprintf(stderr, "could not build the test file\n");
		return (1);
	}

	/* Inside data, and at the start of data. */
	expect(fd, 0, SEEK_DATA, 0, 0);
	expect(fd, 100, SEEK_DATA, 100, 0);
	expect(fd, 0, SEEK_HOLE, 128 * K, 0);
	expect(fd, 100, SEEK_HOLE, 128 * K, 0);

	/* Inside a hole. */
	expect(fd, 128 * K, SEEK_DATA, 8 * M, 0);
	expect(fd, 4 * M, SEEK_DATA, 8 * M, 0);
	expect(fd, 128 * K, SEEK_HOLE, 128 * K, 0);
	expect(fd, 4 * M, SEEK_HOLE, 4 * M, 0);

	/* The second extent, and the hole to EOF. */
	expect(fd, 8 * M, SEEK_DATA, 8 * M, 0);
	expect(fd, 8 * M, SEEK_HOLE, 8 * M + 128 * K, 0);
	expect(fd, 8 * M + 128 * K, SEEK_DATA, ERR, ENXIO);
	expect(fd, 8 * M + 128 * K, SEEK_HOLE, 8 * M + 128 * K, 0);
	expect(fd, 12 * M, SEEK_DATA, ERR, ENXIO);

	/* At and beyond EOF. */
	expect(fd, 16 * M, SEEK_DATA, ERR, ENXIO);
	expect(fd, 16 * M, SEEK_HOLE, ERR, ENXIO);
	expect(fd, 20 * M, SEEK_DATA, ERR, ENXIO);

	(void) close(fd);

	if (failures == 0) {
		(void) printf("seektest: all checks passed\n");
		(void) unlink(argv[1]);
		return (0);
	}

	(void) printf("seektest: %d check(s) failed\n", failures);
	return (2);
}
