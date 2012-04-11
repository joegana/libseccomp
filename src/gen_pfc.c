/**
 * Seccomp Pseudo Filter Code (PFC) Generator
 *
 * Copyright (c) 2012 Red Hat <pmoore@redhat.com>
 * Author: Paul Moore <pmoore@redhat.com>
 */

/*
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of version 2.1 of the GNU Lesser General Public License as
 * published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, see <http://www.gnu.org/licenses>.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <seccomp.h>

#include "arch.h"
#include "db.h"
#include "gen_pfc.h"

struct pfc_sys_list {
	struct db_sys_list *sys;
	struct pfc_sys_list *next;
};

/* XXX - we should check the fprintf() return values */

/**
 * Display a string representation of the node argument
 * @param fds the file stream to send the output
 * @param arch the architecture definition
 * @param node the node
 */
static void _pfc_arg(FILE *fds,
		     const struct arch_def *arch,
		     const struct db_arg_chain_tree *node)
{
	if (arch->size == ARCH_SIZE_64) {
		if (arch_arg_offset_hi(arch, node->arg) == node->arg_offset)
			fprintf(fds, "$a%d.hi32", node->arg);
		else
			fprintf(fds, "$a%d.lo32", node->arg);
	} else
		fprintf(fds, "$a%d", node->arg);
}

/**
 * Display a string representation of the filter action
 * @param fds the file stream to send the output
 * @param action the action
 */
static void _pfc_action(FILE *fds, uint32_t action)
{
	switch (action & 0xffff0000) {
	case SCMP_ACT_KILL:
		fprintf(fds, " action KILL;\n");
		break;
	case SCMP_ACT_TRAP:
		fprintf(fds, " action TRAP;\n");
		break;
	case SCMP_ACT_ERRNO(0):
		fprintf(fds, " action ERRNO(%u);\n", (action & 0x0000ffff));
		break;
	case SCMP_ACT_TRACE(0):
		fprintf(fds, " action TRACE(%u);\n", (action & 0x0000ffff));
		break;
	case SCMP_ACT_ALLOW:
		fprintf(fds, " action ALLOW;\n");
		break;
	default:
		fprintf(fds, " action 0x%x;\n", action);
	}
}

/**
 * Indent the output stream
 * @param fds the file stream to send the output
 * @param lvl the indentation level
 *
 * This function indents the output stream with whitespace based on the
 * requested indentation level.
 */
static void _indent(FILE *fds, unsigned int lvl)
{
	while (lvl-- > 0)
		fprintf(fds, " ");
}

/**
 * Generate the pseudo filter code for an argument chain
 * @param arch the architecture definition
 * @param node the head of the argument chain
 * @param lvl the indentation level
 * @param fds the file stream to send the output
 *
 * This function generates the pseudo filter code representation of the given
 * argument chain and writes it to the given output stream.
 *
 */
static void _gen_pfc_chain(const struct arch_def *arch,
			   const struct db_arg_chain_tree *node,
			   unsigned int lvl, FILE *fds)
{
	const struct db_arg_chain_tree *c_iter;

	/* get to the start */
	c_iter = node;
	while (c_iter->lvl_prv != NULL)
		c_iter = c_iter->lvl_prv;

	while (c_iter != NULL) {
		/* comparison operation */
		_indent(fds, lvl);
		fprintf(fds, " if (");
		_pfc_arg(fds, arch, c_iter);
		switch (c_iter->op) {
			case SCMP_CMP_EQ:
				fprintf(fds, " == ");
				break;
			case SCMP_CMP_GE:
				fprintf(fds, " >= ");
				break;
			case SCMP_CMP_GT:
				fprintf(fds, " > ");
				break;
			case SCMP_CMP_MASKED_EQ:
				fprintf(fds, " & 0x%.8x == ", c_iter->mask);
				break;
			default:
				fprintf(fds, " ??? ");
		}
		fprintf(fds, "%u)\n", c_iter->datum);

		/* true result */
		if (c_iter->act_t_flg) {
			_indent(fds, lvl + 1);
			_pfc_action(fds, c_iter->act_t);
		} else if (c_iter->nxt_t != NULL)
			_gen_pfc_chain(arch, c_iter->nxt_t, lvl + 1, fds);

		/* false result */
		if (c_iter->act_f_flg) {
			_indent(fds, lvl);
			fprintf(fds, " else\n");
			_indent(fds, lvl + 1);
			_pfc_action(fds, c_iter->act_f);
		} else if (c_iter->nxt_f != NULL) {
			_indent(fds, lvl);
			fprintf(fds, " else\n");
			_gen_pfc_chain(arch, c_iter->nxt_f, lvl + 1, fds);
		}

		c_iter = c_iter->lvl_nxt;
	}
}

/**
 * Generate pseudo filter code for a syscall
 * @param arch the architecture definition
 * @param sys the syscall filter
 * @param fds the file stream to send the output
 *
 * This function generates a pseduo filter code representation of the given
 * syscall filter and writes it to the given output stream.
 *
 */
static void _gen_pfc_syscall(const struct arch_def *arch,
			     const struct db_sys_list *sys, FILE *fds)
{
	unsigned int sys_num = sys->num;

	fprintf(fds, "# filter code for syscall #%d (priority: %d)\n",
		sys_num, sys->priority);
	if (sys->chains != NULL) {
		fprintf(fds, " if ($syscall != %d) goto syscal_%d_end;\n",
			sys_num, sys_num);
		_gen_pfc_chain(arch, sys->chains, 0, fds);
		fprintf(fds, " syscall_%d_end:\n", sys_num);
	} else {
		fprintf(fds, " if ($syscall == %d)", sys_num);
		_pfc_action(fds, sys->action);
	}
}

/**
 * Generate a pseudo filter code string representation
 * @param db the seccomp filter DB
 * @param fd the fd to send the output
 *
 * This function generates a pseudo filter code representation of the given
 * filter DB and writes it to the given fd.  Returns zero on success, negative
 * values on failure.
 *
 */
int gen_pfc_generate(const struct db_filter *db, int fd)
{
	int rc = 0;
	int newfd;
	FILE *fds;
	struct db_sys_list *s_iter;
	struct pfc_sys_list *p_iter = NULL, *p_new, *p_head = NULL, *p_prev;

	newfd = dup(fd);
	if (newfd < 0)
		return errno;
	fds = fdopen(newfd, "a");
	if (fds == NULL) {
		close(newfd);
		return errno;
	}

	/* sort the syscall list */
	db_list_foreach(s_iter, db->syscalls) {
		p_new = malloc(sizeof(*p_new));
		if (p_new == NULL) {
			rc = -ENOMEM;
			goto generate_return;
		}
		memset(p_new, 0, sizeof(*p_new));
		p_new->sys = s_iter;

		p_prev = NULL;
		p_iter = p_head;
		while (p_iter != NULL &&
		       s_iter->priority < p_iter->sys->priority) {
			p_prev = p_iter;
			p_iter = p_iter->next;
		}
		if (p_head == NULL)
			p_head = p_new;
		else if (p_prev == NULL) {
			p_new->next = p_head;
			p_head = p_new;
		} else {
			p_new->next = p_iter;
			p_prev->next = p_new;
		}
	}

	/* generate the pfc */
	fprintf(fds, "#\n");
	fprintf(fds, "# pseudo filter code start\n");
	fprintf(fds, "#\n");
	p_iter = p_head;
	while (p_iter != NULL) {
		if (p_iter->sys->valid == 0)
			continue;
		_gen_pfc_syscall(db->arch, p_iter->sys, fds);
		p_iter = p_iter->next;
	}
	fprintf(fds, "# default action\n");
	_pfc_action(fds, db->attr.act_default);
	fprintf(fds, "#\n");
	fprintf(fds, "# pseudo filter code end\n");
	fprintf(fds, "#\n");

	fflush(fds);
	fclose(fds);

generate_return:
	while (p_head != NULL) {
		p_iter = p_head;
		p_head = p_head->next;
		free(p_iter);
	}
	return rc;
}
