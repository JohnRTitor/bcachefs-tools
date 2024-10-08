#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "cmds.h"
#include "libbcachefs.h"
#include "tools-util.h"

#include "libbcachefs/bcachefs.h"
#include "libbcachefs/btree_iter.h"
#include "libbcachefs/errcode.h"
#include "libbcachefs/error.h"
#include "libbcachefs/sb-members.h"
#include "libbcachefs/super.h"

static void kill_btree_node_usage(void)
{
	puts("bcachefs kill_btree_node - make btree nodes unreadable\n"
	     "Usage: bcachefs kill_btree_node [OPTION]... <devices>\n"
	     "\n"
	     "Options:\n"
	     "  -b (extents|inodes|dirents|xattrs)    Btree to delete from\n"
	     "  -l level                              Levle to delete from (0 == leaves)\n"
	     "  -i index                              Index of btree node to kill\n"
	     "  -h                                    Display this help and exit\n"
	     "Report bugs to <linux-bcachefs@vger.kernel.org>");
}

int cmd_kill_btree_node(int argc, char *argv[])
{
	struct bch_opts opts = bch2_opts_empty();
	enum btree_id btree_id = 0;
	unsigned level = 0;
	u64 node_index = 0;
	int opt;

	opt_set(opts, read_only,	true);

	while ((opt = getopt(argc, argv, "b:l:i:h")) != -1)
		switch (opt) {
		case 'b':
			btree_id = read_string_list_or_die(optarg,
						__bch2_btree_ids, "btree id");
			break;
		case 'l':
			if (kstrtouint(optarg, 10, &level) || level >= BTREE_MAX_DEPTH)
				die("invalid level");
			break;
		case 'i':
			if (kstrtoull(optarg, 10, &node_index))
				die("invalid index %s", optarg);
			break;
		case 'h':
			kill_btree_node_usage();
			exit(EXIT_SUCCESS);
		}
	args_shift(optind);

	if (!argc)
		die("Please supply device(s)");

	struct bch_fs *c = bch2_fs_open(argv, argc, opts);
	if (IS_ERR(c))
		die("error opening %s: %s", argv[0], bch2_err_str(PTR_ERR(c)));

	int ret;
	void *zeroes;

	ret = posix_memalign(&zeroes, c->opts.block_size, c->opts.block_size);
	if (ret)
		die("error %s from posix_memalign", bch2_err_str(ret));

	ret = bch2_trans_run(c,
		__for_each_btree_node(trans, iter, btree_id, POS_MIN, 0, level, 0, b, ({
			if (b->c.level != level)
				continue;

			int ret2 = 0;
			if (!node_index) {
				struct printbuf buf = PRINTBUF;
				bch2_bkey_val_to_text(&buf, c, bkey_i_to_s_c(&b->key));
				bch_info(c, "killing btree node %s", buf.buf);
				printbuf_exit(&buf);

				ret2 = 1;

				struct bkey_ptrs_c ptrs = bch2_bkey_ptrs_c(bkey_i_to_s_c(&b->key));
				bkey_for_each_ptr(ptrs, ptr) {
					struct bch_dev *ca = bch2_dev_tryget(c, ptr->dev);
					if (!ca)
						continue;

					int ret3 = pwrite(ca->disk_sb.bdev->bd_fd, zeroes,
						     c->opts.block_size, ptr->offset << 9);
					bch2_dev_put(ca);
					if (ret3 != c->opts.block_size) {
						bch_err(c, "pwrite error: expected %u got %i %s",
							c->opts.block_size, ret, strerror(errno));
						ret2 = EXIT_FAILURE;
					}
				}
			}

			node_index--;
			ret2;
		})));
	if (ret < 0)
		bch_err(c, "error %i walking btree nodes", ret);
	else if (!ret) {
		bch_err(c, "node at specified index not found");
		ret = EXIT_FAILURE;
	}

	bch2_fs_stop(c);
	return ret < 0 ? ret : 0;
}
