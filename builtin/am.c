/*
 * Builtin "git am"
 *
 * Based on git-am.sh by Junio C Hamano.
 */
#include "cache.h"
#include "builtin.h"
#include "exec_cmd.h"
#include "parse-options.h"
#include "dir.h"
#include "run-command.h"
#include "quote.h"
#include "lockfile.h"
#include "cache-tree.h"
#include "refs.h"
#include "commit.h"
#include "diff.h"
#include "diffcore.h"
#include "unpack-trees.h"
#include "branch.h"
#include "sequencer.h"
#include "revision.h"
#include "merge-recursive.h"
#include "revision.h"
#include "log-tree.h"
#include "notes-utils.h"
#include "rerere.h"
#include "prompt.h"

/**
 * Returns 1 if the file is empty or does not exist, 0 otherwise.
 */
static int is_empty_file(const char *filename)
{
	struct stat st;

	if (stat(filename, &st) < 0) {
		if (errno == ENOENT)
			return 1;
		die_errno(_("could not stat %s"), filename);
	}

	return !st.st_size;
}

/**
 * Like strbuf_getline(), but treats both '\n' and "\r\n" as line terminators.
 */
static int strbuf_getline_crlf(struct strbuf *sb, FILE *fp)
{
	if (strbuf_getwholeline(sb, fp, '\n'))
		return EOF;
	if (sb->buf[sb->len - 1] == '\n') {
		strbuf_setlen(sb, sb->len - 1);
		if (sb->len > 0 && sb->buf[sb->len - 1] == '\r')
			strbuf_setlen(sb, sb->len - 1);
	}
	return 0;
}

/**
 * Returns the length of the first line of msg.
 */
static int linelen(const char *msg)
{
	return strchrnul(msg, '\n') - msg;
}

/**
 * Returns true if `str` consists of only whitespace, false otherwise.
 */
static int str_isspace(const char *str)
{
	for (; *str; str++)
		if (!isspace(*str))
			return 0;

	return 1;
}

enum patch_format {
	PATCH_FORMAT_UNKNOWN = 0,
	PATCH_FORMAT_MBOX,
	PATCH_FORMAT_STGIT,
	PATCH_FORMAT_STGIT_SERIES,
	PATCH_FORMAT_HG
};

enum keep_type {
	KEEP_FALSE = 0,
	KEEP_TRUE,      /* pass -k flag to git-mailinfo */
	KEEP_NON_PATCH  /* pass -b flag to git-mailinfo */
};

enum scissors_type {
	SCISSORS_UNSET = -1,
	SCISSORS_FALSE = 0,  /* pass --no-scissors to git-mailinfo */
	SCISSORS_TRUE        /* pass --scissors to git-mailinfo */
};

struct am_state {
	/* state directory path */
	char *dir;

	/* current and last patch numbers, 1-indexed */
	int cur;
	int last;

	/* commit metadata and message */
	char *author_name;
	char *author_email;
	char *author_date;
	char *msg;
	size_t msg_len;

	/* when --rebasing, records the original commit the patch came from */
	unsigned char orig_commit[GIT_SHA1_RAWSZ];

	/* number of digits in patch filename */
	int prec;

	int interactive;

	int threeway;

	int quiet;

	int append_signoff;

	int utf8;

	/* one of the enum keep_type values */
	int keep;

	/* pass -m flag to git-mailinfo */
	int message_id;

	/* one of the enum scissors_type values */
	int scissors;

	struct argv_array git_apply_opts;

	/* override error message when patch failure occurs */
	const char *resolvemsg;

	int committer_date_is_author_date;

	int ignore_date;

	int allow_rerere_autoupdate;

	const char *sign_commit;

	int rebasing;
};

/**
 * Initializes am_state with the default values. The state directory is set to
 * dir.
 */
static void am_state_init(struct am_state *state, const char *dir)
{
	int gpgsign;

	memset(state, 0, sizeof(*state));

	assert(dir);
	state->dir = xstrdup(dir);

	state->prec = 4;

	git_config_get_bool("am.threeway", &state->threeway);

	state->utf8 = 1;

	git_config_get_bool("am.messageid", &state->message_id);

	state->scissors = SCISSORS_UNSET;

	argv_array_init(&state->git_apply_opts);

	if (!git_config_get_bool("commit.gpgsign", &gpgsign))
		state->sign_commit = gpgsign ? "" : NULL;
}

/**
 * Releases memory allocated by an am_state.
 */
static void am_state_release(struct am_state *state)
{
	if (state->dir)
		free(state->dir);

	if (state->author_name)
		free(state->author_name);

	if (state->author_email)
		free(state->author_email);

	if (state->author_date)
		free(state->author_date);

	if (state->msg)
		free(state->msg);

	argv_array_clear(&state->git_apply_opts);
}

/**
 * Returns path relative to the am_state directory.
 */
static inline const char *am_path(const struct am_state *state, const char *path)
{
	assert(state->dir);
	assert(path);
	return mkpath("%s/%s", state->dir, path);
}

/**
 * If state->quiet is false, calls fprintf(fp, fmt, ...), and appends a newline
 * at the end.
 */
static void say(const struct am_state *state, FILE *fp, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (!state->quiet) {
		vfprintf(fp, fmt, ap);
		putc('\n', fp);
	}
	va_end(ap);
}

/**
 * Returns 1 if there is an am session in progress, 0 otherwise.
 */
static int am_in_progress(const struct am_state *state)
{
	struct stat st;

	if (lstat(state->dir, &st) < 0 || !S_ISDIR(st.st_mode))
		return 0;
	if (lstat(am_path(state, "last"), &st) || !S_ISREG(st.st_mode))
		return 0;
	if (lstat(am_path(state, "next"), &st) || !S_ISREG(st.st_mode))
		return 0;
	return 1;
}

/**
 * Reads the contents of `file` in the `state` directory into `sb`. Returns the
 * number of bytes read on success, -1 if the file does not exist. If `trim` is
 * set, trailing whitespace will be removed.
 */
static int read_state_file(struct strbuf *sb, const struct am_state *state,
			const char *file, int trim)
{
	strbuf_reset(sb);

	if (strbuf_read_file(sb, am_path(state, file), 0) >= 0) {
		if (trim)
			strbuf_trim(sb);

		return sb->len;
	}

	if (errno == ENOENT)
		return -1;

	die_errno(_("could not read '%s'"), am_path(state, file));
}

/**
 * Reads a KEY=VALUE shell variable assignment from `fp`, returning the VALUE
 * as a newly-allocated string. VALUE must be a quoted string, and the KEY must
 * match `key`. Returns NULL on failure.
 *
 * This is used by read_author_script() to read the GIT_AUTHOR_* variables from
 * the author-script.
 */
static char *read_shell_var(FILE *fp, const char *key)
{
	struct strbuf sb = STRBUF_INIT;
	const char *str;

	if (strbuf_getline(&sb, fp, '\n'))
		goto fail;

	if (!skip_prefix(sb.buf, key, &str))
		goto fail;

	if (!skip_prefix(str, "=", &str))
		goto fail;

	strbuf_remove(&sb, 0, str - sb.buf);

	str = sq_dequote(sb.buf);
	if (!str)
		goto fail;

	return strbuf_detach(&sb, NULL);

fail:
	strbuf_release(&sb);
	return NULL;
}

/**
 * Reads and parses the state directory's "author-script" file, and sets
 * state->author_name, state->author_email and state->author_date accordingly.
 * Returns 0 on success, -1 if the file could not be parsed.
 *
 * The author script is of the format:
 *
 *	GIT_AUTHOR_NAME='$author_name'
 *	GIT_AUTHOR_EMAIL='$author_email'
 *	GIT_AUTHOR_DATE='$author_date'
 *
 * where $author_name, $author_email and $author_date are quoted. We are strict
 * with our parsing, as the file was meant to be eval'd in the old git-am.sh
 * script, and thus if the file differs from what this function expects, it is
 * better to bail out than to do something that the user does not expect.
 */
static int read_author_script(struct am_state *state)
{
	const char *filename = am_path(state, "author-script");
	FILE *fp;

	assert(!state->author_name);
	assert(!state->author_email);
	assert(!state->author_date);

	fp = fopen(filename, "r");
	if (!fp) {
		if (errno == ENOENT)
			return 0;
		die_errno(_("could not open '%s' for reading"), filename);
	}

	state->author_name = read_shell_var(fp, "GIT_AUTHOR_NAME");
	if (!state->author_name) {
		fclose(fp);
		return -1;
	}

	state->author_email = read_shell_var(fp, "GIT_AUTHOR_EMAIL");
	if (!state->author_email) {
		fclose(fp);
		return -1;
	}

	state->author_date = read_shell_var(fp, "GIT_AUTHOR_DATE");
	if (!state->author_date) {
		fclose(fp);
		return -1;
	}

	if (fgetc(fp) != EOF) {
		fclose(fp);
		return -1;
	}

	fclose(fp);
	return 0;
}

/**
 * Saves state->author_name, state->author_email and state->author_date in the
 * state directory's "author-script" file.
 */
static void write_author_script(const struct am_state *state)
{
	struct strbuf sb = STRBUF_INIT;

	assert(state->author_name);
	assert(state->author_email);
	assert(state->author_date);

	strbuf_addstr(&sb, "GIT_AUTHOR_NAME=");
	sq_quote_buf(&sb, state->author_name);
	strbuf_addch(&sb, '\n');

	strbuf_addstr(&sb, "GIT_AUTHOR_EMAIL=");
	sq_quote_buf(&sb, state->author_email);
	strbuf_addch(&sb, '\n');

	strbuf_addstr(&sb, "GIT_AUTHOR_DATE=");
	sq_quote_buf(&sb, state->author_date);
	strbuf_addch(&sb, '\n');

	write_file(am_path(state, "author-script"), 1, "%s", sb.buf);

	strbuf_release(&sb);
}

/**
 * Reads the commit message from the state directory's "final-commit" file,
 * setting state->msg to its contents and state->msg_len to the length of its
 * contents in bytes.
 *
 * Returns 0 on success, -1 if the file does not exist.
 */
static int read_commit_msg(struct am_state *state)
{
	struct strbuf sb = STRBUF_INIT;

	assert(!state->msg);

	if (read_state_file(&sb, state, "final-commit", 0) < 0) {
		strbuf_release(&sb);
		return -1;
	}

	state->msg = strbuf_detach(&sb, &state->msg_len);
	return 0;
}

/**
 * Saves state->msg in the state directory's "final-commit" file.
 */
static void write_commit_msg(const struct am_state *state)
{
	int fd;
	const char *filename = am_path(state, "final-commit");

	assert(state->msg);

	fd = xopen(filename, O_WRONLY | O_CREAT, 0666);
	if (write_in_full(fd, state->msg, state->msg_len) < 0)
		die_errno(_("could not write to %s"), filename);
	close(fd);
}

/**
 * Loads state from disk.
 */
static void am_load(struct am_state *state)
{
	struct strbuf sb = STRBUF_INIT;

	if (read_state_file(&sb, state, "next", 1) < 0)
		die("BUG: state file 'next' does not exist");
	state->cur = strtol(sb.buf, NULL, 10);

	if (read_state_file(&sb, state, "last", 1) < 0)
		die("BUG: state file 'last' does not exist");
	state->last = strtol(sb.buf, NULL, 10);

	if (read_author_script(state) < 0)
		die(_("could not parse author script"));

	read_commit_msg(state);

	if (read_state_file(&sb, state, "original-commit", 1) < 0)
		hashclr(state->orig_commit);
	else if (get_sha1_hex(sb.buf, state->orig_commit) < 0)
		die(_("could not parse %s"), am_path(state, "original-commit"));

	read_state_file(&sb, state, "threeway", 1);
	state->threeway = !strcmp(sb.buf, "t");

	read_state_file(&sb, state, "quiet", 1);
	state->quiet = !strcmp(sb.buf, "t");

	read_state_file(&sb, state, "sign", 1);
	state->append_signoff = !strcmp(sb.buf, "t");

	read_state_file(&sb, state, "utf8", 1);
	state->utf8 = !strcmp(sb.buf, "t");

	read_state_file(&sb, state, "keep", 1);
	if (!strcmp(sb.buf, "t"))
		state->keep = KEEP_TRUE;
	else if (!strcmp(sb.buf, "b"))
		state->keep = KEEP_NON_PATCH;
	else
		state->keep = KEEP_FALSE;

	read_state_file(&sb, state, "messageid", 1);
	state->message_id = !strcmp(sb.buf, "t");

	read_state_file(&sb, state, "scissors", 1);
	if (!strcmp(sb.buf, "t"))
		state->scissors = SCISSORS_TRUE;
	else if (!strcmp(sb.buf, "f"))
		state->scissors = SCISSORS_FALSE;
	else
		state->scissors = SCISSORS_UNSET;

	read_state_file(&sb, state, "apply-opt", 1);
	argv_array_clear(&state->git_apply_opts);
	if (sq_dequote_to_argv_array(sb.buf, &state->git_apply_opts) < 0)
		die(_("could not parse %s"), am_path(state, "apply-opt"));

	state->rebasing = !!file_exists(am_path(state, "rebasing"));

	strbuf_release(&sb);
}

/**
 * Removes the am_state directory, forcefully terminating the current am
 * session.
 */
static void am_destroy(const struct am_state *state)
{
	struct strbuf sb = STRBUF_INIT;

	strbuf_addstr(&sb, state->dir);
	remove_dir_recursively(&sb, 0);
	strbuf_release(&sb);
}

/**
 * Runs applypatch-msg hook. Returns its exit code.
 */
static int run_applypatch_msg_hook(struct am_state *state)
{
	int ret;

	assert(state->msg);
	ret = run_hook_le(NULL, "applypatch-msg", am_path(state, "final-commit"), NULL);

	if (!ret) {
		free(state->msg);
		state->msg = NULL;
		if (read_commit_msg(state) < 0)
			die(_("'%s' was deleted by the applypatch-msg hook"),
				am_path(state, "final-commit"));
	}

	return ret;
}

/**
 * Runs post-rewrite hook. Returns it exit code.
 */
static int run_post_rewrite_hook(const struct am_state *state)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	const char *hook = find_hook("post-rewrite");
	int ret;

	if (!hook)
		return 0;

	argv_array_push(&cp.args, hook);
	argv_array_push(&cp.args, "rebase");

	cp.in = xopen(am_path(state, "rewritten"), O_RDONLY);
	cp.stdout_to_stderr = 1;

	ret = run_command(&cp);

	close(cp.in);
	return ret;
}

/**
 * Reads the state directory's "rewritten" file, and copies notes from the old
 * commits listed in the file to their rewritten commits.
 *
 * Returns 0 on success, -1 on failure.
 */
static int copy_notes_for_rebase(const struct am_state *state)
{
	struct notes_rewrite_cfg *c;
	struct strbuf sb = STRBUF_INIT;
	const char *invalid_line = _("Malformed input line: '%s'.");
	const char *msg = "Notes added by 'git rebase'";
	FILE *fp;
	int ret = 0;

	assert(state->rebasing);

	c = init_copy_notes_for_rewrite("rebase");
	if (!c)
		return 0;

	fp = xfopen(am_path(state, "rewritten"), "r");

	while (!strbuf_getline(&sb, fp, '\n')) {
		unsigned char from_obj[GIT_SHA1_RAWSZ], to_obj[GIT_SHA1_RAWSZ];

		if (sb.len != GIT_SHA1_HEXSZ * 2 + 1) {
			ret = error(invalid_line, sb.buf);
			goto finish;
		}

		if (get_sha1_hex(sb.buf, from_obj)) {
			ret = error(invalid_line, sb.buf);
			goto finish;
		}

		if (sb.buf[GIT_SHA1_HEXSZ] != ' ') {
			ret = error(invalid_line, sb.buf);
			goto finish;
		}

		if (get_sha1_hex(sb.buf + GIT_SHA1_HEXSZ + 1, to_obj)) {
			ret = error(invalid_line, sb.buf);
			goto finish;
		}

		if (copy_note_for_rewrite(c, from_obj, to_obj))
			ret = error(_("Failed to copy notes from '%s' to '%s'"),
					sha1_to_hex(from_obj), sha1_to_hex(to_obj));
	}

finish:
	finish_copy_notes_for_rewrite(c, msg);
	fclose(fp);
	strbuf_release(&sb);
	return ret;
}

/**
 * Determines if the file looks like a piece of RFC2822 mail by grabbing all
 * non-indented lines and checking if they look like they begin with valid
 * header field names.
 *
 * Returns 1 if the file looks like a piece of mail, 0 otherwise.
 */
static int is_mail(FILE *fp)
{
	const char *header_regex = "^[!-9;-~]+:";
	struct strbuf sb = STRBUF_INIT;
	regex_t regex;
	int ret = 1;

	if (fseek(fp, 0L, SEEK_SET))
		die_errno(_("fseek failed"));

	if (regcomp(&regex, header_regex, REG_NOSUB | REG_EXTENDED))
		die("invalid pattern: %s", header_regex);

	while (!strbuf_getline_crlf(&sb, fp)) {
		if (!sb.len)
			break; /* End of header */

		/* Ignore indented folded lines */
		if (*sb.buf == '\t' || *sb.buf == ' ')
			continue;

		/* It's a header if it matches header_regex */
		if (regexec(&regex, sb.buf, 0, NULL, 0)) {
			ret = 0;
			goto done;
		}
	}

done:
	regfree(&regex);
	strbuf_release(&sb);
	return ret;
}

/**
 * Attempts to detect the patch_format of the patches contained in `paths`,
 * returning the PATCH_FORMAT_* enum value. Returns PATCH_FORMAT_UNKNOWN if
 * detection fails.
 */
static int detect_patch_format(const char **paths)
{
	enum patch_format ret = PATCH_FORMAT_UNKNOWN;
	struct strbuf l1 = STRBUF_INIT;
	struct strbuf l2 = STRBUF_INIT;
	struct strbuf l3 = STRBUF_INIT;
	FILE *fp;

	/*
	 * We default to mbox format if input is from stdin and for directories
	 */
	if (!*paths || !strcmp(*paths, "-") || is_directory(*paths))
		return PATCH_FORMAT_MBOX;

	/*
	 * Otherwise, check the first few lines of the first patch, starting
	 * from the first non-blank line, to try to detect its format.
	 */

	fp = xfopen(*paths, "r");

	while (!strbuf_getline_crlf(&l1, fp)) {
		if (l1.len)
			break;
	}

	if (starts_with(l1.buf, "From ") || starts_with(l1.buf, "From: ")) {
		ret = PATCH_FORMAT_MBOX;
		goto done;
	}

	if (starts_with(l1.buf, "# This series applies on GIT commit")) {
		ret = PATCH_FORMAT_STGIT_SERIES;
		goto done;
	}

	if (!strcmp(l1.buf, "# HG changeset patch")) {
		ret = PATCH_FORMAT_HG;
		goto done;
	}

	strbuf_reset(&l2);
	strbuf_getline_crlf(&l2, fp);
	strbuf_reset(&l3);
	strbuf_getline_crlf(&l3, fp);

	/*
	 * If the second line is empty and the third is a From, Author or Date
	 * entry, this is likely an StGit patch.
	 */
	if (l1.len && !l2.len &&
		(starts_with(l3.buf, "From:") ||
		 starts_with(l3.buf, "Author:") ||
		 starts_with(l3.buf, "Date:"))) {
		ret = PATCH_FORMAT_STGIT;
		goto done;
	}

	if (l1.len && is_mail(fp)) {
		ret = PATCH_FORMAT_MBOX;
		goto done;
	}

done:
	fclose(fp);
	strbuf_release(&l1);
	return ret;
}

/**
 * Splits out individual email patches from `paths`, where each path is either
 * a mbox file or a Maildir. Returns 0 on success, -1 on failure.
 */
static int split_mail_mbox(struct am_state *state, const char **paths, int keep_cr)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	struct strbuf last = STRBUF_INIT;

	cp.git_cmd = 1;
	argv_array_push(&cp.args, "mailsplit");
	argv_array_pushf(&cp.args, "-d%d", state->prec);
	argv_array_pushf(&cp.args, "-o%s", state->dir);
	argv_array_push(&cp.args, "-b");
	if (keep_cr)
		argv_array_push(&cp.args, "--keep-cr");
	argv_array_push(&cp.args, "--");
	argv_array_pushv(&cp.args, paths);

	if (capture_command(&cp, &last, 8))
		return -1;

	state->cur = 1;
	state->last = strtol(last.buf, NULL, 10);

	return 0;
}

/**
 * Callback signature for split_mail_conv(). The foreign patch should be
 * read from `in`, and the converted patch (in RFC2822 mail format) should be
 * written to `out`. Return 0 on success, or -1 on failure.
 */
typedef int (*mail_conv_fn)(FILE *out, FILE *in, int keep_cr);

/**
 * Calls `fn` for each file in `paths` to convert the foreign patch to the
 * RFC2822 mail format suitable for parsing with git-mailinfo.
 *
 * Returns 0 on success, -1 on failure.
 */
static int split_mail_conv(mail_conv_fn fn, struct am_state *state,
			const char **paths, int keep_cr)
{
	static const char *stdin_only[] = {"-", NULL};
	int i;

	if (!*paths)
		paths = stdin_only;

	for (i = 0; *paths; paths++, i++) {
		FILE *in, *out;
		const char *mail;
		int ret;

		if (!strcmp(*paths, "-"))
			in = stdin;
		else
			in = fopen(*paths, "r");

		if (!in)
			return error(_("could not open '%s' for reading: %s"),
					*paths, strerror(errno));

		mail = mkpath("%s/%0*d", state->dir, state->prec, i + 1);

		out = fopen(mail, "w");
		if (!out)
			return error(_("could not open '%s' for writing: %s"),
					mail, strerror(errno));

		ret = fn(out, in, keep_cr);

		fclose(out);
		fclose(in);

		if (ret)
			return error(_("could not parse patch '%s'"), *paths);
	}

	state->cur = 1;
	state->last = i;
	return 0;
}

/**
 * A split_mail_conv() callback that converts an StGit patch to an RFC2822
 * message suitable for parsing with git-mailinfo.
 */
static int stgit_patch_to_mail(FILE *out, FILE *in, int keep_cr)
{
	struct strbuf sb = STRBUF_INIT;
	int subject_printed = 0;

	while (!strbuf_getline(&sb, in, '\n')) {
		const char *str;

		if (str_isspace(sb.buf))
			continue;
		else if (skip_prefix(sb.buf, "Author:", &str))
			fprintf(out, "From:%s\n", str);
		else if (starts_with(sb.buf, "From") || starts_with(sb.buf, "Date"))
			fprintf(out, "%s\n", sb.buf);
		else if (!subject_printed) {
			fprintf(out, "Subject: %s\n", sb.buf);
			subject_printed = 1;
		} else {
			fprintf(out, "\n%s\n", sb.buf);
			break;
		}
	}

	strbuf_reset(&sb);
	while (strbuf_fread(&sb, 8192, in) > 0) {
		fwrite(sb.buf, 1, sb.len, out);
		strbuf_reset(&sb);
	}

	strbuf_release(&sb);
	return 0;
}

/**
 * This function only supports a single StGit series file in `paths`.
 *
 * Given an StGit series file, converts the StGit patches in the series into
 * RFC2822 messages suitable for parsing with git-mailinfo, and queues them in
 * the state directory.
 *
 * Returns 0 on success, -1 on failure.
 */
static int split_mail_stgit_series(struct am_state *state, const char **paths,
					int keep_cr)
{
	const char *series_dir;
	char *series_dir_buf;
	FILE *fp;
	struct argv_array patches = ARGV_ARRAY_INIT;
	struct strbuf sb = STRBUF_INIT;
	int ret;

	if (!paths[0] || paths[1])
		return error(_("Only one StGIT patch series can be applied at once"));

	series_dir_buf = xstrdup(*paths);
	series_dir = dirname(series_dir_buf);

	fp = fopen(*paths, "r");
	if (!fp)
		return error(_("could not open '%s' for reading: %s"), *paths,
				strerror(errno));

	while (!strbuf_getline(&sb, fp, '\n')) {
		if (*sb.buf == '#')
			continue; /* skip comment lines */

		argv_array_push(&patches, mkpath("%s/%s", series_dir, sb.buf));
	}

	fclose(fp);
	strbuf_release(&sb);
	free(series_dir_buf);

	ret = split_mail_conv(stgit_patch_to_mail, state, patches.argv, keep_cr);

	argv_array_clear(&patches);
	return ret;
}

/**
 * A split_patches_conv() callback that converts a mercurial patch to a RFC2822
 * message suitable for parsing with git-mailinfo.
 */
static int hg_patch_to_mail(FILE *out, FILE *in, int keep_cr)
{
	struct strbuf sb = STRBUF_INIT;

	while (!strbuf_getline(&sb, in, '\n')) {
		const char *str;

		if (skip_prefix(sb.buf, "# User ", &str))
			fprintf(out, "From: %s\n", str);
		else if (skip_prefix(sb.buf, "# Date ", &str)) {
			unsigned long timestamp;
			long tz, tz2;
			char *end;

			errno = 0;
			timestamp = strtoul(str, &end, 10);
			if (errno)
				return error(_("invalid timestamp"));

			if (!skip_prefix(end, " ", &str))
				return error(_("invalid Date line"));

			errno = 0;
			tz = strtol(str, &end, 10);
			if (errno)
				return error(_("invalid timezone offset"));

			if (*end)
				return error(_("invalid Date line"));

			/*
			 * mercurial's timezone is in seconds west of UTC,
			 * however git's timezone is in hours + minutes east of
			 * UTC. Convert it.
			 */
			tz2 = labs(tz) / 3600 * 100 + labs(tz) % 3600 / 60;
			if (tz > 0)
				tz2 = -tz2;

			fprintf(out, "Date: %s\n", show_date(timestamp, tz2, DATE_RFC2822));
		} else if (starts_with(sb.buf, "# ")) {
			continue;
		} else {
			fprintf(out, "\n%s\n", sb.buf);
			break;
		}
	}

	strbuf_reset(&sb);
	while (strbuf_fread(&sb, 8192, in) > 0) {
		fwrite(sb.buf, 1, sb.len, out);
		strbuf_reset(&sb);
	}

	strbuf_release(&sb);
	return 0;
}

/**
 * Splits a list of files/directories into individual email patches. Each path
 * in `paths` must be a file/directory that is formatted according to
 * `patch_format`.
 *
 * Once split out, the individual email patches will be stored in the state
 * directory, with each patch's filename being its index, padded to state->prec
 * digits.
 *
 * state->cur will be set to the index of the first mail, and state->last will
 * be set to the index of the last mail.
 *
 * Set keep_cr to 0 to convert all lines ending with \r\n to end with \n, 1
 * to disable this behavior, -1 to use the default configured setting.
 *
 * Returns 0 on success, -1 on failure.
 */
static int split_mail(struct am_state *state, enum patch_format patch_format,
			const char **paths, int keep_cr)
{
	if (keep_cr < 0) {
		keep_cr = 0;
		git_config_get_bool("am.keepcr", &keep_cr);
	}

	switch (patch_format) {
	case PATCH_FORMAT_MBOX:
		return split_mail_mbox(state, paths, keep_cr);
	case PATCH_FORMAT_STGIT:
		return split_mail_conv(stgit_patch_to_mail, state, paths, keep_cr);
	case PATCH_FORMAT_STGIT_SERIES:
		return split_mail_stgit_series(state, paths, keep_cr);
	case PATCH_FORMAT_HG:
		return split_mail_conv(hg_patch_to_mail, state, paths, keep_cr);
	default:
		die("BUG: invalid patch_format");
	}
	return -1;
}

/**
 * Setup a new am session for applying patches
 */
static void am_setup(struct am_state *state, enum patch_format patch_format,
			const char **paths, int keep_cr)
{
	unsigned char curr_head[GIT_SHA1_RAWSZ];
	const char *str;
	struct strbuf sb = STRBUF_INIT;

	if (!patch_format)
		patch_format = detect_patch_format(paths);

	if (!patch_format) {
		fprintf_ln(stderr, _("Patch format detection failed."));
		exit(128);
	}

	if (mkdir(state->dir, 0777) < 0 && errno != EEXIST)
		die_errno(_("failed to create directory '%s'"), state->dir);

	if (split_mail(state, patch_format, paths, keep_cr) < 0) {
		am_destroy(state);
		die(_("Failed to split patches."));
	}

	if (state->rebasing)
		state->threeway = 1;

	write_file(am_path(state, "threeway"), 1, state->threeway ? "t" : "f");

	write_file(am_path(state, "quiet"), 1, state->quiet ? "t" : "f");

	write_file(am_path(state, "sign"), 1, state->append_signoff ? "t" : "f");

	write_file(am_path(state, "utf8"), 1, state->utf8 ? "t" : "f");

	switch (state->keep) {
	case KEEP_FALSE:
		str = "f";
		break;
	case KEEP_TRUE:
		str = "t";
		break;
	case KEEP_NON_PATCH:
		str = "b";
		break;
	default:
		die("BUG: invalid value for state->keep");
	}

	write_file(am_path(state, "keep"), 1, "%s", str);

	write_file(am_path(state, "messageid"), 1, state->message_id ? "t" : "f");

	switch (state->scissors) {
	case SCISSORS_UNSET:
		str = "";
		break;
	case SCISSORS_FALSE:
		str = "f";
		break;
	case SCISSORS_TRUE:
		str = "t";
		break;
	default:
		die("BUG: invalid value for state->scissors");
	}

	write_file(am_path(state, "scissors"), 1, "%s", str);

	sq_quote_argv(&sb, state->git_apply_opts.argv, 0);
	write_file(am_path(state, "apply-opt"), 1, "%s", sb.buf);

	if (state->rebasing)
		write_file(am_path(state, "rebasing"), 1, "%s", "");
	else
		write_file(am_path(state, "applying"), 1, "%s", "");

	if (!get_sha1("HEAD", curr_head)) {
		write_file(am_path(state, "abort-safety"), 1, "%s", sha1_to_hex(curr_head));
		if (!state->rebasing)
			update_ref("am", "ORIG_HEAD", curr_head, NULL, 0,
					UPDATE_REFS_DIE_ON_ERR);
	} else {
		write_file(am_path(state, "abort-safety"), 1, "%s", "");
		if (!state->rebasing)
			delete_ref("ORIG_HEAD", NULL, 0);
	}

	/*
	 * NOTE: Since the "next" and "last" files determine if an am_state
	 * session is in progress, they should be written last.
	 */

	write_file(am_path(state, "next"), 1, "%d", state->cur);

	write_file(am_path(state, "last"), 1, "%d", state->last);

	strbuf_release(&sb);
}

/**
 * Increments the patch pointer, and cleans am_state for the application of the
 * next patch.
 */
static void am_next(struct am_state *state)
{
	unsigned char head[GIT_SHA1_RAWSZ];

	if (state->author_name)
		free(state->author_name);
	state->author_name = NULL;

	if (state->author_email)
		free(state->author_email);
	state->author_email = NULL;

	if (state->author_date)
		free(state->author_date);
	state->author_date = NULL;

	if (state->msg)
		free(state->msg);
	state->msg = NULL;
	state->msg_len = 0;

	unlink(am_path(state, "author-script"));
	unlink(am_path(state, "final-commit"));

	hashclr(state->orig_commit);
	unlink(am_path(state, "original-commit"));

	if (!get_sha1("HEAD", head))
		write_file(am_path(state, "abort-safety"), 1, "%s", sha1_to_hex(head));
	else
		write_file(am_path(state, "abort-safety"), 1, "%s", "");

	state->cur++;
	write_file(am_path(state, "next"), 1, "%d", state->cur);
}

/**
 * Returns the filename of the current patch email.
 */
static const char *msgnum(const struct am_state *state)
{
	static struct strbuf sb = STRBUF_INIT;

	strbuf_reset(&sb);
	strbuf_addf(&sb, "%0*d", state->prec, state->cur);

	return sb.buf;
}

/**
 * Refresh and write index.
 */
static void refresh_and_write_cache(void)
{
	static struct lock_file lock_file;

	hold_locked_index(&lock_file, 1);
	refresh_cache(REFRESH_QUIET);
	if (write_locked_index(&the_index, &lock_file, COMMIT_LOCK))
		die(_("unable to write index file"));
	rollback_lock_file(&lock_file);
}

/**
 * Returns 1 if the index differs from HEAD, 0 otherwise. When on an unborn
 * branch, returns 1 if there are entries in the index, 0 otherwise. If an
 * strbuf is provided, the space-separated list of files that differ will be
 * appended to it.
 */
static int index_has_changes(struct strbuf *sb)
{
	unsigned char head[GIT_SHA1_RAWSZ];
	int i;

	if (!get_sha1_tree("HEAD", head)) {
		struct diff_options opt;

		diff_setup(&opt);
		DIFF_OPT_SET(&opt, EXIT_WITH_STATUS);
		if (!sb)
			DIFF_OPT_SET(&opt, QUICK);
		do_diff_cache(head, &opt);
		diffcore_std(&opt);
		for (i = 0; sb && i < diff_queued_diff.nr; i++) {
			if (i)
				strbuf_addch(sb, ' ');
			strbuf_addstr(sb, diff_queued_diff.queue[i]->two->path);
		}
		diff_flush(&opt);
		return DIFF_OPT_TST(&opt, HAS_CHANGES) != 0;
	} else {
		for (i = 0; sb && i < active_nr; i++) {
			if (i)
				strbuf_addch(sb, ' ');
			strbuf_addstr(sb, active_cache[i]->name);
		}
		return !!active_nr;
	}
}

/**
 * Dies with a user-friendly message on how to proceed after resolving the
 * problem. This message can be overridden with state->resolvemsg.
 */
static void NORETURN die_user_resolve(const struct am_state *state)
{
	if (state->resolvemsg) {
		printf_ln("%s", state->resolvemsg);
	} else {
		const char *cmdline = state->interactive ? "git am -i" : "git am";

		printf_ln(_("When you have resolved this problem, run \"%s --continue\"."), cmdline);
		printf_ln(_("If you prefer to skip this patch, run \"%s --skip\" instead."), cmdline);
		printf_ln(_("To restore the original branch and stop patching, run \"%s --abort\"."), cmdline);
	}

	exit(128);
}

/**
 * Parses `mail` using git-mailinfo, extracting its patch and authorship info.
 * state->msg will be set to the patch message. state->author_name,
 * state->author_email and state->author_date will be set to the patch author's
 * name, email and date respectively. The patch body will be written to the
 * state directory's "patch" file.
 *
 * Returns 1 if the patch should be skipped, 0 otherwise.
 */
static int parse_mail(struct am_state *state, const char *mail)
{
	FILE *fp;
	struct child_process cp = CHILD_PROCESS_INIT;
	struct strbuf sb = STRBUF_INIT;
	struct strbuf msg = STRBUF_INIT;
	struct strbuf author_name = STRBUF_INIT;
	struct strbuf author_date = STRBUF_INIT;
	struct strbuf author_email = STRBUF_INIT;
	int ret = 0;

	cp.git_cmd = 1;
	cp.in = xopen(mail, O_RDONLY, 0);
	cp.out = xopen(am_path(state, "info"), O_WRONLY | O_CREAT, 0777);

	argv_array_push(&cp.args, "mailinfo");
	argv_array_push(&cp.args, state->utf8 ? "-u" : "-n");

	switch (state->keep) {
	case KEEP_FALSE:
		break;
	case KEEP_TRUE:
		argv_array_push(&cp.args, "-k");
		break;
	case KEEP_NON_PATCH:
		argv_array_push(&cp.args, "-b");
		break;
	default:
		die("BUG: invalid value for state->keep");
	}

	if (state->message_id)
		argv_array_push(&cp.args, "-m");

	switch (state->scissors) {
	case SCISSORS_UNSET:
		break;
	case SCISSORS_FALSE:
		argv_array_push(&cp.args, "--no-scissors");
		break;
	case SCISSORS_TRUE:
		argv_array_push(&cp.args, "--scissors");
		break;
	default:
		die("BUG: invalid value for state->scissors");
	}

	argv_array_push(&cp.args, am_path(state, "msg"));
	argv_array_push(&cp.args, am_path(state, "patch"));

	if (run_command(&cp) < 0)
		die("could not parse patch");

	close(cp.in);
	close(cp.out);

	/* Extract message and author information */
	fp = xfopen(am_path(state, "info"), "r");
	while (!strbuf_getline(&sb, fp, '\n')) {
		const char *x;

		if (skip_prefix(sb.buf, "Subject: ", &x)) {
			if (msg.len)
				strbuf_addch(&msg, '\n');
			strbuf_addstr(&msg, x);
		} else if (skip_prefix(sb.buf, "Author: ", &x))
			strbuf_addstr(&author_name, x);
		else if (skip_prefix(sb.buf, "Email: ", &x))
			strbuf_addstr(&author_email, x);
		else if (skip_prefix(sb.buf, "Date: ", &x))
			strbuf_addstr(&author_date, x);
	}
	fclose(fp);

	/* Skip pine's internal folder data */
	if (!strcmp(author_name.buf, "Mail System Internal Data")) {
		ret = 1;
		goto finish;
	}

	if (is_empty_file(am_path(state, "patch"))) {
		printf_ln(_("Patch is empty. Was it split wrong?"));
		die_user_resolve(state);
	}

	strbuf_addstr(&msg, "\n\n");
	if (strbuf_read_file(&msg, am_path(state, "msg"), 0) < 0)
		die_errno(_("could not read '%s'"), am_path(state, "msg"));
	stripspace(&msg, 0);

	if (state->append_signoff)
		append_signoff(&msg, 0, 0);

	free(state->author_name);
	state->author_name = strbuf_detach(&author_name, NULL);

	free(state->author_email);
	state->author_email = strbuf_detach(&author_email, NULL);

	free(state->author_date);
	state->author_date = strbuf_detach(&author_date, NULL);

	free(state->msg);
	state->msg = strbuf_detach(&msg, &state->msg_len);

finish:
	strbuf_release(&msg);
	strbuf_release(&author_date);
	strbuf_release(&author_email);
	strbuf_release(&author_name);
	strbuf_release(&sb);
	return ret;
}

/**
 * Sets commit_id to the commit hash where the mail was generated from.
 * Returns 0 on success, -1 on failure.
 */
static int get_mail_commit_sha1(unsigned char *commit_id, const char *mail)
{
	struct strbuf sb = STRBUF_INIT;
	FILE *fp = xfopen(mail, "r");
	const char *x;

	if (strbuf_getline(&sb, fp, '\n'))
		return -1;

	if (!skip_prefix(sb.buf, "From ", &x))
		return -1;

	if (get_sha1_hex(x, commit_id) < 0)
		return -1;

	strbuf_release(&sb);
	fclose(fp);
	return 0;
}

/**
 * Sets state->msg, state->author_name, state->author_email, state->author_date
 * to the commit's respective info.
 */
static void get_commit_info(struct am_state *state, struct commit *commit)
{
	const char *buffer, *ident_line, *author_date, *msg;
	size_t ident_len;
	struct ident_split ident_split;
	struct strbuf sb = STRBUF_INIT;

	buffer = logmsg_reencode(commit, NULL, get_commit_output_encoding());

	ident_line = find_commit_header(buffer, "author", &ident_len);

	if (split_ident_line(&ident_split, ident_line, ident_len) < 0) {
		strbuf_add(&sb, ident_line, ident_len);
		die(_("invalid ident line: %s"), sb.buf);
	}

	free(state->author_name);
	if (ident_split.name_begin) {
		strbuf_add(&sb, ident_split.name_begin,
			ident_split.name_end - ident_split.name_begin);
		state->author_name = strbuf_detach(&sb, NULL);
	} else
		state->author_name = xstrdup("");

	free(state->author_email);
	if (ident_split.mail_begin) {
		strbuf_add(&sb, ident_split.mail_begin,
			ident_split.mail_end - ident_split.mail_begin);
		state->author_email = strbuf_detach(&sb, NULL);
	} else
		state->author_email = xstrdup("");

	author_date = show_ident_date(&ident_split, DATE_NORMAL);
	strbuf_addstr(&sb, author_date);
	free(state->author_date);
	state->author_date = strbuf_detach(&sb, NULL);

	msg = strstr(buffer, "\n\n");
	if (!msg)
		die(_("unable to parse commit %s"), sha1_to_hex(commit->object.sha1));
	free(state->msg);
	state->msg = xstrdup(msg + 2);
	state->msg_len = strlen(state->msg);
}

/**
 * Writes `commit` as a patch to the state directory's "patch" file.
 */
static void write_commit_patch(const struct am_state *state, struct commit *commit)
{
	struct rev_info rev_info;
	FILE *fp;

	fp = xfopen(am_path(state, "patch"), "w");
	init_revisions(&rev_info, NULL);
	rev_info.diff = 1;
	rev_info.abbrev = 0;
	rev_info.disable_stdin = 1;
	rev_info.show_root_diff = 1;
	rev_info.diffopt.output_format = DIFF_FORMAT_PATCH;
	rev_info.no_commit_id = 1;
	DIFF_OPT_SET(&rev_info.diffopt, BINARY);
	DIFF_OPT_SET(&rev_info.diffopt, FULL_INDEX);
	rev_info.diffopt.use_color = 0;
	rev_info.diffopt.file = fp;
	rev_info.diffopt.close_file = 1;
	add_pending_object(&rev_info, &commit->object, "");
	diff_setup_done(&rev_info.diffopt);
	log_tree_commit(&rev_info, commit);
}

/**
 * Writes the diff of the index against HEAD as a patch to the state
 * directory's "patch" file.
 */
static void write_index_patch(const struct am_state *state)
{
	struct tree *tree;
	unsigned char head[GIT_SHA1_RAWSZ];
	struct rev_info rev_info;
	FILE *fp;

	if (!get_sha1_tree("HEAD", head))
		tree = lookup_tree(head);
	else
		tree = lookup_tree(EMPTY_TREE_SHA1_BIN);

	fp = xfopen(am_path(state, "patch"), "w");
	init_revisions(&rev_info, NULL);
	rev_info.diff = 1;
	rev_info.disable_stdin = 1;
	rev_info.no_commit_id = 1;
	rev_info.diffopt.output_format = DIFF_FORMAT_PATCH;
	rev_info.diffopt.use_color = 0;
	rev_info.diffopt.file = fp;
	rev_info.diffopt.close_file = 1;
	add_pending_object(&rev_info, &tree->object, "");
	diff_setup_done(&rev_info.diffopt);
	run_diff_index(&rev_info, 1);
}

/**
 * Like parse_mail(), but parses the mail by looking up its commit ID
 * directly. This is used in --rebasing mode to bypass git-mailinfo's munging
 * of patches.
 *
 * state->orig_commit will be set to the original commit ID.
 *
 * Will always return 0 as the patch should never be skipped.
 */
static int parse_mail_rebase(struct am_state *state, const char *mail)
{
	struct commit *commit;
	unsigned char commit_sha1[GIT_SHA1_RAWSZ];

	if (get_mail_commit_sha1(commit_sha1, mail) < 0)
		die(_("could not parse %s"), mail);

	commit = lookup_commit_or_die(commit_sha1, mail);

	get_commit_info(state, commit);

	write_commit_patch(state, commit);

	hashcpy(state->orig_commit, commit_sha1);
	write_file(am_path(state, "original-commit"), 1, "%s",
			sha1_to_hex(commit_sha1));

	return 0;
}

/**
 * Applies current patch with git-apply. Returns 0 on success, -1 otherwise. If
 * `index_file` is not NULL, the patch will be applied to that index.
 */
static int run_apply(const struct am_state *state, const char *index_file)
{
	struct child_process cp = CHILD_PROCESS_INIT;

	cp.git_cmd = 1;

	if (index_file)
		argv_array_pushf(&cp.env_array, "GIT_INDEX_FILE=%s", index_file);

	/*
	 * If we are allowed to fall back on 3-way merge, don't give false
	 * errors during the initial attempt.
	 */
	if (state->threeway && !index_file) {
		cp.no_stdout = 1;
		cp.no_stderr = 1;
	}

	argv_array_push(&cp.args, "apply");

	argv_array_pushv(&cp.args, state->git_apply_opts.argv);

	if (index_file)
		argv_array_push(&cp.args, "--cached");
	else
		argv_array_push(&cp.args, "--index");

	argv_array_push(&cp.args, am_path(state, "patch"));

	if (run_command(&cp))
		return -1;

	/* Reload index as git-apply will have modified it. */
	discard_cache();
	read_cache_from(index_file ? index_file : get_index_file());

	return 0;
}

/**
 * Builds an index that contains just the blobs needed for a 3way merge.
 */
static int build_fake_ancestor(const struct am_state *state, const char *index_file)
{
	struct child_process cp = CHILD_PROCESS_INIT;

	cp.git_cmd = 1;
	argv_array_push(&cp.args, "apply");
	argv_array_pushv(&cp.args, state->git_apply_opts.argv);
	argv_array_pushf(&cp.args, "--build-fake-ancestor=%s", index_file);
	argv_array_push(&cp.args, am_path(state, "patch"));

	if (run_command(&cp))
		return -1;

	return 0;
}

/**
 * Attempt a threeway merge, using index_path as the temporary index.
 */
static int fall_back_threeway(const struct am_state *state, const char *index_path)
{
	unsigned char orig_tree[GIT_SHA1_RAWSZ], his_tree[GIT_SHA1_RAWSZ],
		      our_tree[GIT_SHA1_RAWSZ];
	const unsigned char *bases[1] = {orig_tree};
	struct merge_options o;
	struct commit *result;
	char *his_tree_name;

	if (get_sha1("HEAD", our_tree) < 0)
		hashcpy(our_tree, EMPTY_TREE_SHA1_BIN);

	if (build_fake_ancestor(state, index_path))
		return error("could not build fake ancestor");

	discard_cache();
	read_cache_from(index_path);

	if (write_index_as_tree(orig_tree, &the_index, index_path, 0, NULL))
		return error(_("Repository lacks necessary blobs to fall back on 3-way merge."));

	say(state, stdout, _("Using index info to reconstruct a base tree..."));

	if (!state->quiet) {
		/*
		 * List paths that needed 3-way fallback, so that the user can
		 * review them with extra care to spot mismerges.
		 */
		struct rev_info rev_info;
		const char *diff_filter_str = "--diff-filter=AM";

		init_revisions(&rev_info, NULL);
		rev_info.diffopt.output_format = DIFF_FORMAT_NAME_STATUS;
		diff_opt_parse(&rev_info.diffopt, &diff_filter_str, 1);
		add_pending_sha1(&rev_info, "HEAD", our_tree, 0);
		diff_setup_done(&rev_info.diffopt);
		run_diff_index(&rev_info, 1);
	}

	if (run_apply(state, index_path))
		return error(_("Did you hand edit your patch?\n"
				"It does not apply to blobs recorded in its index."));

	if (write_index_as_tree(his_tree, &the_index, index_path, 0, NULL))
		return error("could not write tree");

	say(state, stdout, _("Falling back to patching base and 3-way merge..."));

	discard_cache();
	read_cache();

	/*
	 * This is not so wrong. Depending on which base we picked, orig_tree
	 * may be wildly different from ours, but his_tree has the same set of
	 * wildly different changes in parts the patch did not touch, so
	 * recursive ends up canceling them, saying that we reverted all those
	 * changes.
	 */

	init_merge_options(&o);

	o.branch1 = "HEAD";
	his_tree_name = xstrfmt("%.*s", linelen(state->msg), state->msg);
	o.branch2 = his_tree_name;

	if (state->quiet)
		o.verbosity = 0;

	if (merge_recursive_generic(&o, our_tree, his_tree, 1, bases, &result)) {
		rerere(state->allow_rerere_autoupdate);
		free(his_tree_name);
		return error(_("Failed to merge in the changes."));
	}

	free(his_tree_name);
	return 0;
}

/**
 * Commits the current index with state->msg as the commit message and
 * state->author_name, state->author_email and state->author_date as the author
 * information.
 */
static void do_commit(const struct am_state *state)
{
	unsigned char tree[GIT_SHA1_RAWSZ], parent[GIT_SHA1_RAWSZ],
		      commit[GIT_SHA1_RAWSZ];
	unsigned char *ptr;
	struct commit_list *parents = NULL;
	const char *reflog_msg, *author;
	struct strbuf sb = STRBUF_INIT;

	if (run_hook_le(NULL, "pre-applypatch", NULL))
		exit(1);

	if (write_cache_as_tree(tree, 0, NULL))
		die(_("git write-tree failed to write a tree"));

	if (!get_sha1_commit("HEAD", parent)) {
		ptr = parent;
		commit_list_insert(lookup_commit(parent), &parents);
	} else {
		ptr = NULL;
		say(state, stderr, _("applying to an empty history"));
	}

	author = fmt_ident(state->author_name, state->author_email,
			state->ignore_date ? NULL : state->author_date,
			IDENT_STRICT);

	if (state->committer_date_is_author_date)
		setenv("GIT_COMMITTER_DATE",
			state->ignore_date ? "" : state->author_date, 1);

	if (commit_tree(state->msg, state->msg_len, tree, parents, commit,
				author, state->sign_commit))
		die(_("failed to write commit object"));

	reflog_msg = getenv("GIT_REFLOG_ACTION");
	if (!reflog_msg)
		reflog_msg = "am";

	strbuf_addf(&sb, "%s: %.*s", reflog_msg, linelen(state->msg),
			state->msg);

	update_ref(sb.buf, "HEAD", commit, ptr, 0, UPDATE_REFS_DIE_ON_ERR);

	if (state->rebasing) {
		FILE *fp = xfopen(am_path(state, "rewritten"), "a");

		assert(!is_null_sha1(state->orig_commit));
		fprintf(fp, "%s ", sha1_to_hex(state->orig_commit));
		fprintf(fp, "%s\n", sha1_to_hex(commit));
		fclose(fp);
	}

	run_hook_le(NULL, "post-applypatch", NULL);

	strbuf_release(&sb);
}

/**
 * Interactively prompt the user on whether the current patch should be
 * applied.
 *
 * Returns 0 if the user chooses to apply the patch, 1 if the user chooses to
 * skip it.
 */
static int do_interactive(struct am_state *state)
{
	assert(state->msg);

	if (!isatty(0))
		die(_("cannot be interactive without stdin connected to a terminal."));

	for (;;) {
		const char *reply;

		puts(_("Commit Body is:"));
		puts("--------------------------");
		printf("%s", state->msg);
		puts("--------------------------");

		/*
		 * TRANSLATORS: Make sure to include [y], [n], [e], [v] and [a]
		 * in your translation. The program will only accept English
		 * input at this point.
		 */
		reply = git_prompt(_("Apply? [y]es/[n]o/[e]dit/[v]iew patch/[a]ccept all: "), PROMPT_ECHO);

		if (!reply) {
			continue;
		} else if (*reply == 'y' || *reply == 'Y') {
			return 0;
		} else if (*reply == 'a' || *reply == 'A') {
			state->interactive = 0;
			return 0;
		} else if (*reply == 'n' || *reply == 'N') {
			return 1;
		} else if (*reply == 'e' || *reply == 'E') {
			struct strbuf msg = STRBUF_INIT;

			if (!launch_editor(am_path(state, "final-commit"), &msg, NULL)) {
				free(state->msg);
				state->msg = strbuf_detach(&msg, &state->msg_len);
			}
			strbuf_release(&msg);
		} else if (*reply == 'v' || *reply == 'V') {
			const char *pager = git_pager(1);
			struct child_process cp = CHILD_PROCESS_INIT;

			if (!pager)
				pager = "cat";
			argv_array_push(&cp.args, pager);
			argv_array_push(&cp.args, am_path(state, "patch"));
			run_command(&cp);
		}
	}
}

/**
 * Applies all queued mail.
 */
static void am_run(struct am_state *state)
{
	const char *argv_gc_auto[] = {"gc", "--auto", NULL};
	struct strbuf sb = STRBUF_INIT;

	unlink(am_path(state, "dirtyindex"));

	refresh_and_write_cache();

	if (index_has_changes(&sb)) {
		write_file(am_path(state, "dirtyindex"), 1, "t");
		die(_("Dirty index: cannot apply patches (dirty: %s)"), sb.buf);
	}

	strbuf_release(&sb);

	while (state->cur <= state->last) {
		const char *mail = am_path(state, msgnum(state));
		int apply_status, skip;

		if (!file_exists(mail))
			goto next;

		if (state->rebasing)
			skip = parse_mail_rebase(state, mail);
		else
			skip = parse_mail(state, mail);

		if (skip)
			goto next; /* mail should be skipped */

		write_author_script(state);
		write_commit_msg(state);

		if (state->interactive && do_interactive(state))
			goto next;

		if (run_applypatch_msg_hook(state))
			exit(1);

		say(state, stdout, _("Applying: %.*s"), linelen(state->msg), state->msg);

		apply_status = run_apply(state, NULL);

		if (apply_status && state->threeway) {
			struct strbuf sb = STRBUF_INIT;

			strbuf_addstr(&sb, am_path(state, "patch-merge-index"));
			apply_status = fall_back_threeway(state, sb.buf);
			strbuf_release(&sb);

			/*
			 * Applying the patch to an earlier tree and merging
			 * the result may have produced the same tree as ours.
			 */
			if (!apply_status && !index_has_changes(NULL)) {
				say(state, stdout, _("No changes -- Patch already applied."));
				goto next;
			}
		}

		if (apply_status) {
			int advice_amworkdir = 1;

			printf_ln(_("Patch failed at %s %.*s"), msgnum(state),
				linelen(state->msg), state->msg);

			git_config_get_bool("advice.amworkdir", &advice_amworkdir);

			if (advice_amworkdir)
				printf_ln(_("The copy of the patch that failed is found in: %s"),
						am_path(state, "patch"));

			die_user_resolve(state);
		}

		do_commit(state);

next:
		am_next(state);
	}

	if (!is_empty_file(am_path(state, "rewritten"))) {
		assert(state->rebasing);
		copy_notes_for_rebase(state);
		run_post_rewrite_hook(state);
	}

	/*
	 * In rebasing mode, it's up to the caller to take care of
	 * housekeeping.
	 */
	if (!state->rebasing) {
		am_destroy(state);
		run_command_v_opt(argv_gc_auto, RUN_GIT_CMD);
	}
}

/**
 * Resume the current am session after patch application failure. The user did
 * all the hard work, and we do not have to do any patch application. Just
 * trust and commit what the user has in the index and working tree.
 */
static void am_resolve(struct am_state *state)
{
	if (!state->msg)
		die(_("cannot resume: %s does not exist."),
			am_path(state, "final-commit"));

	if (!state->author_name || !state->author_email || !state->author_date)
		die(_("cannot resume: %s does not exist."),
			am_path(state, "author-script"));

	say(state, stdout, _("Applying: %.*s"), linelen(state->msg), state->msg);

	if (!index_has_changes(NULL)) {
		printf_ln(_("No changes - did you forget to use 'git add'?\n"
			"If there is nothing left to stage, chances are that something else\n"
			"already introduced the same changes; you might want to skip this patch."));
		die_user_resolve(state);
	}

	if (unmerged_cache()) {
		printf_ln(_("You still have unmerged paths in your index.\n"
			"Did you forget to use 'git add'?"));
		die_user_resolve(state);
	}

	if (state->interactive) {
		write_index_patch(state);
		if (do_interactive(state))
			goto next;
	}

	rerere(0);

	do_commit(state);

next:
	am_next(state);
	am_run(state);
}

/**
 * Performs a checkout fast-forward from `head` to `remote`. If `reset` is
 * true, any unmerged entries will be discarded. Returns 0 on success, -1 on
 * failure.
 */
static int fast_forward_to(struct tree *head, struct tree *remote, int reset)
{
	struct lock_file *lock_file = xcalloc(1, sizeof(struct lock_file));
	struct unpack_trees_options opts;
	struct tree_desc t[2];

	if (parse_tree(head) || parse_tree(remote))
		return -1;

	hold_locked_index(lock_file, 1);

	refresh_cache(REFRESH_QUIET);

	memset(&opts, 0, sizeof(opts));
	opts.head_idx = 1;
	opts.src_index = &the_index;
	opts.dst_index = &the_index;
	opts.update = 1;
	opts.merge = 1;
	opts.reset = reset;
	opts.fn = twoway_merge;
	init_tree_desc(&t[0], head->buffer, head->size);
	init_tree_desc(&t[1], remote->buffer, remote->size);

	if (unpack_trees(2, t, &opts)) {
		rollback_lock_file(lock_file);
		return -1;
	}

	if (write_locked_index(&the_index, lock_file, COMMIT_LOCK))
		die(_("unable to write new index file"));

	return 0;
}

/**
 * Clean the index without touching entries that are not modified between
 * `head` and `remote`.
 */
static int clean_index(const unsigned char *head, const unsigned char *remote)
{
	struct lock_file *lock_file = xcalloc(1, sizeof(struct lock_file));
	struct tree *head_tree, *remote_tree, *index_tree;
	unsigned char index[GIT_SHA1_RAWSZ];
	struct pathspec pathspec;

	head_tree = parse_tree_indirect(head);
	if (!head_tree)
		return error(_("Could not parse object '%s'."), sha1_to_hex(head));

	remote_tree = parse_tree_indirect(remote);
	if (!remote_tree)
		return error(_("Could not parse object '%s'."), sha1_to_hex(remote));

	read_cache_unmerged();

	if (fast_forward_to(head_tree, head_tree, 1))
		return -1;

	if (write_cache_as_tree(index, 0, NULL))
		return -1;

	index_tree = parse_tree_indirect(index);
	if (!index_tree)
		return error(_("Could not parse object '%s'."), sha1_to_hex(index));

	if (fast_forward_to(index_tree, remote_tree, 0))
		return -1;

	memset(&pathspec, 0, sizeof(pathspec));

	hold_locked_index(lock_file, 1);

	if (read_tree(remote_tree, 0, &pathspec)) {
		rollback_lock_file(lock_file);
		return -1;
	}

	if (write_locked_index(&the_index, lock_file, COMMIT_LOCK))
		die(_("unable to write new index file"));

	remove_branch_state();

	return 0;
}

/**
 * Resets rerere's merge resolution metadata.
 */
static void am_rerere_clear(void)
{
	struct string_list merge_rr = STRING_LIST_INIT_DUP;
	int fd = setup_rerere(&merge_rr, 0);

	if (fd < 0)
		return;

	rerere_clear(&merge_rr);
	string_list_clear(&merge_rr, 1);
}

/**
 * Resume the current am session by skipping the current patch.
 */
static void am_skip(struct am_state *state)
{
	unsigned char head[GIT_SHA1_RAWSZ];

	am_rerere_clear();

	if (get_sha1("HEAD", head))
		hashcpy(head, EMPTY_TREE_SHA1_BIN);

	if (clean_index(head, head))
		die(_("failed to clean index"));

	am_next(state);
	am_run(state);
}

/**
 * Returns true if it is safe to reset HEAD to the ORIG_HEAD, false otherwise.
 *
 * It is not safe to reset HEAD when:
 * 1. git-am previously failed because the index was dirty.
 * 2. HEAD has moved since git-am previously failed.
 */
static int safe_to_abort(const struct am_state *state)
{
	struct strbuf sb = STRBUF_INIT;
	unsigned char abort_safety[GIT_SHA1_RAWSZ], head[GIT_SHA1_RAWSZ];

	if (file_exists(am_path(state, "dirtyindex")))
		return 0;

	if (read_state_file(&sb, state, "abort-safety", 1) > 0) {
		if (get_sha1_hex(sb.buf, abort_safety))
			die(_("could not parse %s"), am_path(state, "abort_safety"));
	} else
		hashclr(abort_safety);

	if (get_sha1("HEAD", head))
		hashclr(head);

	if (!hashcmp(head, abort_safety))
		return 1;

	error(_("You seem to have moved HEAD since the last 'am' failure.\n"
		"Not rewinding to ORIG_HEAD"));

	return 0;
}

/**
 * Aborts the current am session if it is safe to do so.
 */
static void am_abort(struct am_state *state)
{
	unsigned char curr_head[GIT_SHA1_RAWSZ], orig_head[GIT_SHA1_RAWSZ];
	int has_curr_head, has_orig_head;
	const char *curr_branch;

	if (!safe_to_abort(state)) {
		am_destroy(state);
		return;
	}

	am_rerere_clear();

	curr_branch = resolve_refdup("HEAD", 0, curr_head, NULL);
	has_curr_head = !is_null_sha1(curr_head);
	if (!has_curr_head)
		hashcpy(curr_head, EMPTY_TREE_SHA1_BIN);

	has_orig_head = !get_sha1("ORIG_HEAD", orig_head);
	if (!has_orig_head)
		hashcpy(orig_head, EMPTY_TREE_SHA1_BIN);

	clean_index(curr_head, orig_head);

	if (has_orig_head)
		update_ref("am --abort", "HEAD", orig_head,
				has_curr_head ? curr_head : NULL, 0,
				UPDATE_REFS_DIE_ON_ERR);
	else if (curr_branch)
		delete_ref(curr_branch, NULL, REF_NODEREF);

	am_destroy(state);
}

/**
 * parse_options() callback that validates and sets opt->value to the
 * PATCH_FORMAT_* enum value corresponding to `arg`.
 */
static int parse_opt_patchformat(const struct option *opt, const char *arg, int unset)
{
	int *opt_value = opt->value;

	if (!strcmp(arg, "mbox"))
		*opt_value = PATCH_FORMAT_MBOX;
	else if (!strcmp(arg, "stgit"))
		*opt_value = PATCH_FORMAT_STGIT;
	else if (!strcmp(arg, "stgit-series"))
		*opt_value = PATCH_FORMAT_STGIT_SERIES;
	else if (!strcmp(arg, "hg"))
		*opt_value = PATCH_FORMAT_HG;
	else
		return error(_("Invalid value for --patch-format: %s"), arg);
	return 0;
}

enum resume_mode {
	RESUME_FALSE = 0,
	RESUME_RESOLVED,
	RESUME_SKIP,
	RESUME_ABORT
};

int cmd_am(int argc, const char **argv, const char *prefix)
{
	struct am_state state;
	int binary = -1;
	int keep_cr = -1;
	int patch_format = PATCH_FORMAT_UNKNOWN;
	enum resume_mode resume = RESUME_FALSE;

	const char * const usage[] = {
		N_("git am [options] [(<mbox>|<Maildir>)...]"),
		N_("git am [options] (--continue | --skip | --abort)"),
		NULL
	};

	struct option options[] = {
		OPT_BOOL('i', "interactive", &state.interactive,
			N_("run interactively")),
		OPT_HIDDEN_BOOL('b', "binary", &binary,
			N_("(historical option -- no-op")),
		OPT_BOOL('3', "3way", &state.threeway,
			N_("allow fall back on 3way merging if needed")),
		OPT__QUIET(&state.quiet, N_("be quiet")),
		OPT_BOOL('s', "signoff", &state.append_signoff,
			N_("add a Signed-off-by line to the commit message")),
		OPT_BOOL('u', "utf8", &state.utf8,
			N_("recode into utf8 (default)")),
		OPT_SET_INT('k', "keep", &state.keep,
			N_("pass -k flag to git-mailinfo"), KEEP_TRUE),
		OPT_SET_INT(0, "keep-non-patch", &state.keep,
			N_("pass -b flag to git-mailinfo"), KEEP_NON_PATCH),
		OPT_BOOL('m', "message-id", &state.message_id,
			N_("pass -m flag to git-mailinfo")),
		{ OPTION_SET_INT, 0, "keep-cr", &keep_cr, NULL,
		  N_("pass --keep-cr flag to git-mailsplit for mbox format"),
		  PARSE_OPT_NOARG | PARSE_OPT_NONEG, NULL, 1},
		{ OPTION_SET_INT, 0, "no-keep-cr", &keep_cr, NULL,
		  N_("do not pass --keep-cr flag to git-mailsplit independent of am.keepcr"),
		  PARSE_OPT_NOARG | PARSE_OPT_NONEG, NULL, 0},
		OPT_BOOL('c', "scissors", &state.scissors,
			N_("strip everything before a scissors line")),
		OPT_PASSTHRU_ARGV(0, "whitespace", &state.git_apply_opts, N_("action"),
			N_("pass it through git-apply"),
			0),
		OPT_PASSTHRU_ARGV(0, "ignore-space-change", &state.git_apply_opts, NULL,
			N_("pass it through git-apply"),
			PARSE_OPT_NOARG),
		OPT_PASSTHRU_ARGV(0, "ignore-whitespace", &state.git_apply_opts, NULL,
			N_("pass it through git-apply"),
			PARSE_OPT_NOARG),
		OPT_PASSTHRU_ARGV(0, "directory", &state.git_apply_opts, N_("root"),
			N_("pass it through git-apply"),
			0),
		OPT_PASSTHRU_ARGV(0, "exclude", &state.git_apply_opts, N_("path"),
			N_("pass it through git-apply"),
			0),
		OPT_PASSTHRU_ARGV(0, "include", &state.git_apply_opts, N_("path"),
			N_("pass it through git-apply"),
			0),
		OPT_PASSTHRU_ARGV('C', NULL, &state.git_apply_opts, N_("n"),
			N_("pass it through git-apply"),
			0),
		OPT_PASSTHRU_ARGV('p', NULL, &state.git_apply_opts, N_("num"),
			N_("pass it through git-apply"),
			0),
		OPT_CALLBACK(0, "patch-format", &patch_format, N_("format"),
			N_("format the patch(es) are in"),
			parse_opt_patchformat),
		OPT_PASSTHRU_ARGV(0, "reject", &state.git_apply_opts, NULL,
			N_("pass it through git-apply"),
			PARSE_OPT_NOARG),
		OPT_STRING(0, "resolvemsg", &state.resolvemsg, NULL,
			N_("override error message when patch failure occurs")),
		OPT_CMDMODE(0, "continue", &resume,
			N_("continue applying patches after resolving a conflict"),
			RESUME_RESOLVED),
		OPT_CMDMODE('r', "resolved", &resume,
			N_("synonyms for --continue"),
			RESUME_RESOLVED),
		OPT_CMDMODE(0, "skip", &resume,
			N_("skip the current patch"),
			RESUME_SKIP),
		OPT_CMDMODE(0, "abort", &resume,
			N_("restore the original branch and abort the patching operation."),
			RESUME_ABORT),
		OPT_BOOL(0, "committer-date-is-author-date",
			&state.committer_date_is_author_date,
			N_("lie about committer date")),
		OPT_BOOL(0, "ignore-date", &state.ignore_date,
			N_("use current timestamp for author date")),
		OPT_RERERE_AUTOUPDATE(&state.allow_rerere_autoupdate),
		{ OPTION_STRING, 'S', "gpg-sign", &state.sign_commit, N_("key-id"),
		  N_("GPG-sign commits"),
		  PARSE_OPT_OPTARG, NULL, (intptr_t) "" },
		OPT_HIDDEN_BOOL(0, "rebasing", &state.rebasing,
			N_("(internal use for git-rebase)")),
		OPT_END()
	};

	/*
	 * NEEDSWORK: Once all the features of git-am.sh have been
	 * re-implemented in builtin/am.c, this preamble can be removed.
	 */
	if (!getenv("_GIT_USE_BUILTIN_AM")) {
		const char *path = mkpath("%s/git-am", git_exec_path());

		if (sane_execvp(path, (char **)argv) < 0)
			die_errno("could not exec %s", path);
	} else {
		prefix = setup_git_directory();
		trace_repo_setup(prefix);
		setup_work_tree();
	}

	git_config(git_default_config, NULL);

	am_state_init(&state, git_path("rebase-apply"));

	argc = parse_options(argc, argv, prefix, options, usage, 0);

	if (binary >= 0)
		fprintf_ln(stderr, _("The -b/--binary option has been a no-op for long time, and\n"
				"it will be removed. Please do not use it anymore."));

	/* Ensure a valid committer ident can be constructed */
	git_committer_info(IDENT_STRICT);

	if (read_index_preload(&the_index, NULL) < 0)
		die(_("failed to read the index"));

	if (am_in_progress(&state)) {
		/*
		 * Catch user error to feed us patches when there is a session
		 * in progress:
		 *
		 * 1. mbox path(s) are provided on the command-line.
		 * 2. stdin is not a tty: the user is trying to feed us a patch
		 *    from standard input. This is somewhat unreliable -- stdin
		 *    could be /dev/null for example and the caller did not
		 *    intend to feed us a patch but wanted to continue
		 *    unattended.
		 */
		if (argc || (resume == RESUME_FALSE && !isatty(0)))
			die(_("previous rebase directory %s still exists but mbox given."),
				state.dir);

		am_load(&state);
	} else {
		struct argv_array paths = ARGV_ARRAY_INIT;
		int i;

		/*
		 * Handle stray state directory in the independent-run case. In
		 * the --rebasing case, it is up to the caller to take care of
		 * stray directories.
		 */
		if (file_exists(state.dir) && !state.rebasing) {
			if (resume == RESUME_ABORT) {
				am_destroy(&state);
				am_state_release(&state);
				return 0;
			}

			die(_("Stray %s directory found.\n"
				"Use \"git am --abort\" to remove it."),
				state.dir);
		}

		if (resume)
			die(_("Resolve operation not in progress, we are not resuming."));

		for (i = 0; i < argc; i++) {
			if (is_absolute_path(argv[i]) || !prefix)
				argv_array_push(&paths, argv[i]);
			else
				argv_array_push(&paths, mkpath("%s/%s", prefix, argv[i]));
		}

		am_setup(&state, patch_format, paths.argv, keep_cr);

		argv_array_clear(&paths);
	}

	switch (resume) {
	case RESUME_FALSE:
		am_run(&state);
		break;
	case RESUME_RESOLVED:
		am_resolve(&state);
		break;
	case RESUME_SKIP:
		am_skip(&state);
		break;
	case RESUME_ABORT:
		am_abort(&state);
		break;
	default:
		die("BUG: invalid resume value");
	}

	am_state_release(&state);

	return 0;
}
