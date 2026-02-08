/* $OpenBSD$ */

/*
 * Copyright (c) 2007 Nicholas Marriott <nicholas.marriott@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <stdlib.h>
#include <string.h>

#include "tmux.h"

/*
 * List key bindings.
 */

#define REPEAT_FMT		\
	"#{?key_repeat,-r,  } "

#define LIST_KEYS_TEMPLATE		\
	"bind-key "			\
	"%s"				\
        "-T #{p%u:key_tablename} "      \
	"#{p%u:key_string} "		\
	"#{key_command}"		\

#define LIST_KEYS_N_FLAG_TEMPLATE			\
	"#{key_prefix} "				\
	"#{p%u:key_string} "				\
	"#{?key_note,#{key_note},#{key_command}}"

static enum cmd_retval cmd_list_keys_exec(struct cmd *, struct cmdq_item *);

const struct cmd_entry cmd_list_keys_entry = {
	.name = "list-keys",
	.alias = "lsk",

	.args = { "1aF:NO:P:rT:", 0, 1, NULL },
	.usage = "[-1aNr] [-F format] [-O order] [-P prefix-string]"
		 "[-T key-table] [key]",

	.flags = CMD_STARTSERVER|CMD_AFTERHOOK,
	.exec = cmd_list_keys_exec
};

static char *
cmd_list_keys_get_prefix(struct args *args)
{
	char		*s;
	key_code	 prefix;

	prefix = options_get_number(global_s_options, "prefix");
	if (!args_has(args, 'P')) {
		if (prefix != KEYC_NONE)
			xasprintf(&s, "%s", key_string_lookup_key(prefix, 0));
		else
			s = xstrdup("");
	} else
		s = xstrdup(args_get(args, 'P'));
	return (s);
}

static int
cmd_list_contains_repeatable_key_binding(struct key_binding **l, u_int n)
{
	u_int	i;

	for (i = 0; i < n; i++)
		if (l[i]->flags & KEY_BINDING_REPEAT)
			return (1);
	return (0);
}

static int
cmd_skip_for_N_flag(struct key_binding *bd, struct args *args,
    const char *tablename)
{
	if (!args_has(args, 'N'))
		return (0);
	if (strcasecmp(bd->tablename, "root") != 0 &&
	    strcasecmp(bd->tablename, "prefix") != 0 &&
	    (args_has(args, 'T') && strcasecmp(bd->tablename, tablename) != 0))
		return (1);
	return (bd->note == NULL || args_has(args, 'a'));
}

static u_int
cmd_list_keys_get_width(struct key_binding **l, u_int n, struct args *args,
    const char *tablename)
{
	struct key_binding	*bd;
	u_int			 i, width, keywidth = 0;

	for (i = 0; i < n; i++) {
		bd = l[i];
		if (cmd_skip_for_N_flag(bd, args, tablename))
			continue;

		width = utf8_cstrwidth(key_string_lookup_key(bd->key, 0));
		if (width > keywidth)
			keywidth = width;
	}
	return (keywidth);
}

static u_int
cmd_list_keys_get_table_width(struct key_binding **l, u_int n)
{
	struct key_binding	*bd;
	u_int			 i, width, tablewidth = 0;

	for (i = 0; i < n; i++) {
		bd = l[i];
		width = utf8_cstrwidth(bd->tablename);
		if (width > tablewidth)
			tablewidth = width;
	}
	return (tablewidth);
}

static void
cmd_format_tree_add_key_binding(struct format_tree *ft,
    const struct key_binding *bd, const char *prefix)
{
	char	*tmp;

	if (bd->flags & KEY_BINDING_REPEAT)
		tmp = xstrdup("1");
	else
		tmp = xstrdup("0");
	format_add(ft, "key_repeat", "%s", tmp);
	if (bd->note != NULL)
		format_add(ft, "key_note", "%s", xstrdup(bd->note));
	format_add(ft, "key_prefix", "%s", xstrdup(prefix));
	format_add(ft, "key_tablename", "%s", xstrdup(bd->tablename));
	format_add(ft, "key_string", "%s",
	    key_string_lookup_key(bd->key, 0));
	format_add(ft, "key_command", "%s",
	    cmd_list_print(bd->cmdlist,
	        CMD_LIST_PRINT_ESCAPED|CMD_LIST_PRINT_NO_GROUPS));
}

static enum cmd_retval
cmd_list_keys_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	struct client		*tc = cmdq_get_target_client(item);
	struct format_tree	*ft;
	struct key_table	*table;
	struct key_binding	*bd, **l;
	key_code		 only = KEYC_UNKNOWN;
	const char		*template, *tablename, *keystr, *line, *repeat;
	static char		 template0[8192];
	char			*prefix = NULL;
	u_int			 i, n, keywidth, tablewidth = 0;
	int			 single;
	struct sort_criteria	 sort_crit;

	if ((keystr = args_string(args, 0)) != NULL) {
		only = key_string_lookup_string(keystr);
		if (only == KEYC_UNKNOWN) {
			cmdq_error(item, "invalid key: %s", keystr);
			return (CMD_RETURN_ERROR);
		}
		only &= (KEYC_MASK_KEY|KEYC_MASK_MODIFIERS);
	}

	sort_crit.order = sort_order_from_string(args_get(args, 'O'));
	sort_crit.reversed = args_has(args, 'r');

	if ((tablename = args_get(args, 'T')) != NULL) {
		table = key_bindings_get_table(tablename, 0);
		if (table == NULL) {
			cmdq_error(item, "table %s doesn't exist", tablename);
			return (CMD_RETURN_ERROR);
		}
		l = sort_get_key_bindings_table(table, only, &n, &sort_crit);
	} else
		l = sort_get_key_bindings(only, &n, &sort_crit);

	if (cmd_list_contains_repeatable_key_binding(l, n))
		repeat = REPEAT_FMT;
	else
		repeat = "";
	if ((template = args_get(args, 'F')) == NULL) {
		keywidth = cmd_list_keys_get_width(l, n, args, tablename);
		if (args_has(args, 'N')) {
			xsnprintf(template0, sizeof template0,
			    LIST_KEYS_N_FLAG_TEMPLATE, keywidth);
		} else {
			tablewidth = cmd_list_keys_get_table_width(l, n);
			xsnprintf(template0, sizeof template0,
			    LIST_KEYS_TEMPLATE, repeat, tablewidth, keywidth);
		}
		template = template0;
	}

	prefix = cmd_list_keys_get_prefix(args);
	single = args_has(args, '1');
	ft = format_create(cmdq_get_client(item), item, FORMAT_NONE, 0);
	format_defaults(ft, NULL, NULL, NULL, NULL);
	for (i = 0; i < n; i++) {
		bd = l[i];
		if (cmd_skip_for_N_flag(bd, args, tablename))
			continue;

		cmd_format_tree_add_key_binding(ft, bd, prefix);
		line = format_expand(ft, template);

		if ((single && tc != NULL) || n == 1)
			status_message_set(tc, -1, 1, 0, 0, "%s", line);
		else {
			if (*line != '\0')
				cmdq_print(item, "%s", line);
		}
		free(line);

		if (single)
			break;
	}
	format_free(ft);
	free(prefix);

	return (CMD_RETURN_NORMAL);
}
