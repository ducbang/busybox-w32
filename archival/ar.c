/* vi: set sw=4 ts=4: */
/*
 * Mini ar implementation for busybox
 *
 * Copyright (C) 2000 by Glenn McGrath
 *
 * Based in part on BusyBox tar, Debian dpkg-deb and GNU ar.
 *
 * Licensed under GPLv2 or later, see file LICENSE in this source tree.
 *
 * Archive creation support:
 * Copyright (C) 2010 Nokia Corporation. All rights reserved.
 * Written by Alexander Shishkin.
 *
 * There is no single standard to adhere to so ar may not portable
 * between different systems
 * http://www.unix-systems.org/single_unix_specification_v2/xcu/ar.html
 */
//config:config AR
//config:	bool "ar (9.5 kb)"
//config:	default n  # needs to be improved to be able to replace binutils ar
//config:	help
//config:	ar is an archival utility program used to create, modify, and
//config:	extract contents from archives. In practice, it is used exclusively
//config:	for object module archives used by compilers.
//config:
//config:	Unless you have a specific application which requires ar, you should
//config:	probably say N here: most compilers come with their own ar utility.
//config:
//config:config FEATURE_AR_LONG_FILENAMES
//config:	bool "Support long filenames (not needed for debs)"
//config:	default y
//config:	depends on AR || MAKE
//config:	help
//config:	By default the ar format can only store the first 15 characters
//config:	of the filename, this option removes that limitation.
//config:	It supports the GNU ar long filename method which moves multiple long
//config:	filenames into the data section of a new ar entry.
//config:
//config:config FEATURE_AR_CREATE
//config:	bool "Support archive creation"
//config:	default y
//config:	depends on AR
//config:	help
//config:	This enables archive creation (-c and -r) with busybox ar.

//applet:IF_AR(APPLET(ar, BB_DIR_USR_BIN, BB_SUID_DROP))

//kbuild:lib-$(CONFIG_AR) += ar.o

#include "libbb.h"
#include "bb_archive.h"
#include "ar_.h"

#if ENABLE_FEATURE_AR_CREATE
/* filter out entries with same names as specified on the command line */
static char FAST_FUNC filter_replaceable(archive_handle_t *handle)
{
	if (find_list_entry(handle->accept, handle->file_header->name))
		return EXIT_FAILURE;

	return EXIT_SUCCESS;
}

static void output_ar_header(archive_handle_t *handle)
{
	/* GNU ar 2.19.51.0.14 creates malformed archives
	 * if input files are >10G. It also truncates files >4GB
	 * (uses "size mod 4G"). We abort in this case:
	 * We could add support for up to 10G files, but this is unlikely to be useful.
	 * Note that unpacking side limits all fields to "unsigned int" data type,
	 * and treats "all ones" as an error indicator. Thus max we allow here is UINT_MAX-1.
	 */
	enum {
		/* for 2nd field: mtime */
		MAX11CHARS = UINT_MAX > 0xffffffff ? (unsigned)99999999999 : UINT_MAX-1,
		/* for last field: filesize */
		MAX10CHARS = UINT_MAX > 0xffffffff ? (unsigned)9999999999 : UINT_MAX-1,
	};

	struct file_header_t *fh = handle->file_header;

	if (handle->offset & 1) {
		xwrite(handle->src_fd, "\n", 1);
		handle->offset++;
	}

	/* Careful! The widths should be exact. Fields must be separated */
	if (sizeof(off_t) > 4 && fh->size > (off_t)MAX10CHARS) {
		bb_error_msg_and_die("'%s' is bigger than ar can handle", fh->name);
	}
	fdprintf(handle->src_fd, "%-16.16s%-12lu%-6u%-6u%-8o%-10"OFF_FMT"u`\n",
			fh->name,
			(sizeof(time_t) > 4 && fh->mtime > MAX11CHARS) ? (long)0 : (long)fh->mtime,
			fh->uid > 99999 ? 0 : (int)fh->uid,
			fh->gid > 99999 ? 0 : (int)fh->gid,
			(int)fh->mode & 07777777,
			fh->size
	);

	handle->offset += AR_HEADER_LEN;
}

/*
 * when replacing files in an existing archive, copy from the
 * original archive those files that are to be left intact
 */
static void FAST_FUNC copy_data(archive_handle_t *handle)
{
	archive_handle_t *out_handle = handle->ar__out;
	struct file_header_t *fh = handle->file_header;

	out_handle->file_header = fh;
	output_ar_header(out_handle);

	bb_copyfd_exact_size(handle->src_fd, out_handle->src_fd, fh->size);
	out_handle->offset += fh->size;
}

static int write_ar_header(archive_handle_t *handle)
{
	char *fn;
	char fn_h[17]; /* 15 + "/" + NUL */
	struct stat st;
	int fd;

	fn = llist_pop(&handle->accept);
	if (!fn)
		return -1;

	xstat(fn, &st);

	handle->file_header->mtime = st.st_mtime;
	handle->file_header->uid = st.st_uid;
	handle->file_header->gid = st.st_gid;
	handle->file_header->mode = st.st_mode;
	handle->file_header->size = st.st_size;
	handle->file_header->name = fn_h;
//TODO: if ENABLE_FEATURE_AR_LONG_FILENAMES...
	sprintf(fn_h, "%.15s/", bb_basename(fn));

	output_ar_header(handle);

	fd = xopen(fn, O_RDONLY);
	bb_copyfd_exact_size(fd, handle->src_fd, st.st_size);
	close(fd);
	handle->offset += st.st_size;

	return 0;
}

static int write_ar_archive(archive_handle_t *handle)
{
	struct stat st;
	archive_handle_t *out_handle;
#if ENABLE_PLATFORM_MINGW32
	char *temp_fn = NULL;
#endif

	xfstat(handle->src_fd, &st, handle->ar__name);

	/* if archive exists, create a new handle for output.
	 * we create it in place of the old one.
	 */
	if (st.st_size != 0) {
		out_handle = init_handle();
#if !ENABLE_PLATFORM_MINGW32
		xunlink(handle->ar__name);
		out_handle->src_fd = xopen(handle->ar__name, O_WRONLY | O_CREAT | O_TRUNC);
#else
		/* can't unlink open file, create temporary output file */
		temp_fn = xasprintf("%sXXXXXX", handle->ar__name);
		out_handle->src_fd = xmkstemp(temp_fn);
#endif
		out_handle->accept = handle->accept;
	} else {
		out_handle = handle;
	}

	handle->ar__out = out_handle;

	xwrite(out_handle->src_fd, AR_MAGIC "\n", AR_MAGIC_LEN + 1);
	out_handle->offset += AR_MAGIC_LEN + 1;

	/* skip to the end of the archive if we have to append stuff */
	if (st.st_size != 0) {
		handle->filter = filter_replaceable;
		handle->action_data = copy_data;
		unpack_ar_archive(handle);
	}

	while (write_ar_header(out_handle) == 0)
		continue;

	/* optional, since we exit right after we return */
	if (ENABLE_FEATURE_CLEAN_UP || ENABLE_PLATFORM_MINGW32) {
		close(handle->src_fd);
		if (out_handle->src_fd != handle->src_fd)
			close(out_handle->src_fd);
	}

#if ENABLE_PLATFORM_MINGW32
	if (temp_fn != NULL) {
		xrename(temp_fn, handle->ar__name);
		if (ENABLE_FEATURE_CLEAN_UP)
			free(temp_fn);
	}
#endif

	return EXIT_SUCCESS;
}
#endif /* FEATURE_AR_CREATE */

static void FAST_FUNC header_verbose_list_ar(const file_header_t *file_header)
{
	char mode[12];
	char *mtime;

	bb_mode_string(mode, file_header->mode);
	mtime = ctime(&file_header->mtime);
	mtime[16] = ' ';
	memmove(&mtime[17], &mtime[20], 4);
	mtime[21] = '\0';
	printf("%s %u/%u%7"OFF_FMT"u %s %s\n", &mode[1],
			(int)file_header->uid, (int)file_header->gid,
			file_header->size,
			&mtime[4], file_header->name
	);
}

//usage:#define ar_trivial_usage
//usage:       "x|p|t"IF_FEATURE_AR_CREATE("|r")" [-ov] ARCHIVE [FILE]..."
//usage:#define ar_full_usage "\n\n"
//usage:       "Extract or list FILEs from an ar archive"IF_FEATURE_AR_CREATE(", or create it")"\n"
//usage:     "\n	x	Extract"
//usage:     "\n	p	Extract to stdout"
//usage:     "\n	t	List"
//usage:	IF_FEATURE_AR_CREATE(
//usage:     "\n	r	Create"
//usage:	)
//usage:     "\n	-o	Restore mtime"
//usage:     "\n	-v	Verbose"

int ar_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int ar_main(int argc UNUSED_PARAM, char **argv)
{
	archive_handle_t *archive_handle;
	unsigned opt, t;
	enum {
		OPT_VERBOSE       = (1 << 0),
		OPT_PRESERVE_DATE = (1 << 1),
		/* "ar r" implies create, but warns about it. c suppresses warning.
		 * bbox accepts but ignores it: */
		OPT_CREATE        = (1 << 2),
		CMD_PRINT         = (1 << 3),
		FIRST_CMD         = CMD_PRINT,
		CMD_LIST          = (1 << 4),
		CMD_EXTRACT       = (1 << 5),
		CMD_INSERT        = ((1 << 6) * ENABLE_FEATURE_AR_CREATE),
	};

	archive_handle = init_handle();

	/* prepend '-' to the first argument if required */
	if (argv[1] && argv[1][0] != '-' && argv[1][0] != '\0')
		argv[1] = xasprintf("-%s", argv[1]);
	opt = getopt32(argv, "^"
		"voc""ptx"IF_FEATURE_AR_CREATE("r")
		"\0"
		/* -1: at least one arg is reqd */
		/* one of p,t,x[,r] is required */
		"-1:p:t:x"IF_FEATURE_AR_CREATE(":r")
	);
	argv += optind;

	t = opt / FIRST_CMD;
	if (t & (t-1)) /* more than one of p,t,x[,r] are specified */
		bb_show_usage();

	if (opt & CMD_PRINT) {
		archive_handle->action_data = data_extract_to_stdout;
	}
	if (opt & CMD_LIST) {
		archive_handle->action_header = header_list;
	}
	if (opt & CMD_EXTRACT) {
		archive_handle->action_data = data_extract_all;
	}
	if (opt & OPT_PRESERVE_DATE) {
		archive_handle->ah_flags |= ARCHIVE_RESTORE_DATE;
	}
	if (opt & OPT_VERBOSE) {
		archive_handle->action_header = header_verbose_list_ar;
	}
#if ENABLE_FEATURE_AR_CREATE
	archive_handle->ar__name = *argv;
#endif
	archive_handle->src_fd = xopen(*argv++,
			(opt & CMD_INSERT)
				? O_RDWR | O_CREAT
				: O_RDONLY
	);

	if (*argv)
		archive_handle->filter = filter_accept_list;
	while (*argv) {
		llist_add_to_end(&archive_handle->accept, *argv++);
	}

#if ENABLE_FEATURE_AR_CREATE
	if (opt & CMD_INSERT)
		return write_ar_archive(archive_handle);
#endif

	unpack_ar_archive(archive_handle);

	return EXIT_SUCCESS;
}
