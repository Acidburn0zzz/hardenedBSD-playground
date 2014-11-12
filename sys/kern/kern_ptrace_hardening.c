/*-
 * Copyright (c) 2014, by David Carlier <devnexen at gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_ptrace_hardening.h"

#include <sys/param.h>
#include <sys/mount.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/imgact.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/malloc.h>
#include <sys/stat.h>
#include <sys/ptrace.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/sbuf.h>
#include <sys/limits.h>
#include <sys/queue.h>
#include <sys/jail.h>
#include <sys/libkern.h>

#include <sys/syslimits.h>
#include <sys/param.h>

#include <sys/ptrace_hardening.h>
#include <sys/pax.h>

#include <machine/stdarg.h>

#include <security/mac_bsdextended/mac_bsdextended.h>

FEATURE(ptrace_hardening, "Ptrace call restrictions.");

static MALLOC_DEFINE(HARDENING_PTRACE, "ptrace hardening",
	"Ptrace Hardening allocations");

int ptrace_hardening_status = PAX_FEATURE_SIMPLE_ENABLED;
int ptrace_hardening_flag_status = PAX_FEATURE_SIMPLE_ENABLED;

#ifdef PTRACE_HARDENING_GRP
gid_t ptrace_hardening_allowed_gid = 0;
#endif

static char ptrace_request_flags[PT_FIRSTMACH + 1] = { 0 };
static int ptrace_request_flags_all = 0;
static struct mtx ptrace_request_flags_mtx;


static void ptrace_hardening_sysinit(void);
static void ptrace_hardening_sysuninit(void);
static struct prison *ptrace_get_prison(struct proc *);

TUNABLE_INT("hardening.ptrace.status", &ptrace_hardening_status);
TUNABLE_INT("hardening.ptrace.flag_status", &ptrace_hardening_flag_status);

#ifdef PTRACE_HARDENING_GRP
TUNABLE_INT("hardening.ptrace.allowed_gid", &ptrace_hardening_allowed_gid);
#endif

#ifdef PTRACE_HARDENING_SYSCTLS
static int sysctl_ptrace_hardening_status(SYSCTL_HANDLER_ARGS);
static int sysctl_ptrace_hardening_flag(SYSCTL_HANDLER_ARGS);
static int sysctl_ptrace_hardening_flagall(SYSCTL_HANDLER_ARGS);

#ifdef PTRACE_HARDENING_GRP
static int sysctl_ptrace_hardening_gid(SYSCTL_HANDLER_ARGS);
#endif	/* PTRACE_HARDENING_GRP */
#endif	/* PTRACE_HARDENING_SYSCTLS */

#ifdef PTRACE_HARDENING_SYSCTLS
#define PTRACE_REQUEST_FLAG_SYSCTL(PTFLAG, name)			\
static int sysctl_ptrace_hardening_##name##_flag(SYSCTL_HANDLER_ARGS);	\										\
SYSCTL_PROC(_hardening_ptrace_flag, OID_AUTO, name,			\
    CTLTYPE_INT|CTLFLAG_RWTUN|CTLFLAG_PRISON|CTLFLAG_SECURE,		\
    NULL, 0, sysctl_ptrace_hardening_##name##_flag, "I",		\
    "Request "#name" flag");						\
									\
int									\
sysctl_ptrace_hardening_##name##_flag(SYSCTL_HANDLER_ARGS)		\
{									\
	struct prison *pr;					\
	int err, val;                   				\
									\
	pr = pax_get_prison(req->td->td_proc);				\
									\
	val = pr->pr_hardening.hr_ptrace_request_flags[PTFLAG];		\
	err = sysctl_handle_int(oidp, &val, sizeof(int), req);		\
	if (err || (req->newptr == NULL))				\
		return (err);						\
									\
	switch (val) {							\
	case PAX_FEATURE_SIMPLE_DISABLED:				\
	case PAX_FEATURE_SIMPLE_ENABLED:				\
		if (pr == &prison0)					\
			ptrace_request_flags[PTFLAG] = val;		\
									\
		prison_lock(pr);					\
		pr->pr_hardening.hr_ptrace_request_flags[PTFLAG] = val;	\
		prison_unlock(pr);					\
		break;							\
	default:							\
		return (EINVAL);					\
	}								\
									\
	return (0);							\
}
#else
#define PTRACE_REQUEST_FLAG_SYSCTL(PTFLAG, name)
#endif	/* PTRACE_HARDENING_SYSCTLS */

#define PTRACE_REQUEST_FLAG(PTFLAG, name)				\
TUNABLE_STR("hardening.ptrace.flag."#name, 				\
    &ptrace_request_flags[PTFLAG], 1);					\
									\
PTRACE_REQUEST_FLAG_SYSCTL(PTFLAG, name)

#ifdef PTRACE_HARDENING_SYSCTLS
SYSCTL_NODE(_hardening, OID_AUTO, ptrace, CTLFLAG_RD, 0,
    "PTrace settings.");

SYSCTL_NODE(_hardening_ptrace, OID_AUTO, flag, CTLFLAG_RD, 0,
    "PTrace request flags settings.");

SYSCTL_PROC(_hardening_ptrace, OID_AUTO, status,
    CTLTYPE_INT|CTLFLAG_RWTUN|CTLFLAG_PRISON|CTLFLAG_SECURE,
    NULL, 0, sysctl_ptrace_hardening_status, "I",
    "Restrictions status. "
    "0 - disabled, "
    "1 - enabled");

SYSCTL_PROC(_hardening_ptrace, OID_AUTO, flag_status,
    CTLTYPE_INT|CTLFLAG_RWTUN|CTLFLAG_PRISON|CTLFLAG_SECURE,
    NULL, 0, sysctl_ptrace_hardening_flag, "I",
    "Flag status");

SYSCTL_PROC(_hardening_ptrace, OID_AUTO, flag_all,
    CTLTYPE_INT|CTLFLAG_RWTUN|CTLFLAG_PRISON|CTLFLAG_SECURE,
    NULL, 0, sysctl_ptrace_hardening_flagall, "I",
    "Flag status");

#ifdef PTRACE_HARDENING_GRP
SYSCTL_PROC(_hardening_ptrace, OID_AUTO, allowed_gid,
    CTLTYPE_ULONG|CTLFLAG_RWTUN|CTLFLAG_PRISON|CTLFLAG_SECURE,
    NULL, 0, sysctl_ptrace_hardening_gid, "LU",
    "Allowed gid");
#endif	/* PTRACE_HARDENING_GRP */
#endif	/* PTRACE_HARDENING_SYSCTLS */

PTRACE_REQUEST_FLAG(PT_TRACE_ME, trace_me)
PTRACE_REQUEST_FLAG(PT_READ_I, read_i)
PTRACE_REQUEST_FLAG(PT_READ_D, read_d)
PTRACE_REQUEST_FLAG(PT_WRITE_I, write_i)
PTRACE_REQUEST_FLAG(PT_WRITE_D, write_d)
PTRACE_REQUEST_FLAG(PT_CONTINUE, continue)
PTRACE_REQUEST_FLAG(PT_KILL, kill)
PTRACE_REQUEST_FLAG(PT_STEP, step)
PTRACE_REQUEST_FLAG(PT_ATTACH, attach)
PTRACE_REQUEST_FLAG(PT_DETACH, detach)
PTRACE_REQUEST_FLAG(PT_IO, io);
PTRACE_REQUEST_FLAG(PT_LWPINFO, lwpinfo)
PTRACE_REQUEST_FLAG(PT_GETNUMLWPS, getnumlwps)
PTRACE_REQUEST_FLAG(PT_GETLWPLIST, getlwplist)
PTRACE_REQUEST_FLAG(PT_CLEARSTEP, clearstep)
PTRACE_REQUEST_FLAG(PT_SETSTEP, setstep)
PTRACE_REQUEST_FLAG(PT_SUSPEND, suspend)
PTRACE_REQUEST_FLAG(PT_RESUME, resume)
PTRACE_REQUEST_FLAG(PT_TO_SCE, to_sce)
PTRACE_REQUEST_FLAG(PT_TO_SCX, to_scx)
PTRACE_REQUEST_FLAG(PT_SYSCALL, syscall)
PTRACE_REQUEST_FLAG(PT_FOLLOW_FORK, follow_fork)
PTRACE_REQUEST_FLAG(PT_GETREGS, getregs)
PTRACE_REQUEST_FLAG(PT_SETREGS, setregs)
PTRACE_REQUEST_FLAG(PT_GETFPREGS, getfpregs)
PTRACE_REQUEST_FLAG(PT_SETFPREGS, setfpregs)
PTRACE_REQUEST_FLAG(PT_GETDBREGS, getdbregs)
PTRACE_REQUEST_FLAG(PT_SETDBREGS, setdbregs)
PTRACE_REQUEST_FLAG(PT_VM_TIMESTAMP, vm_timestamp)
PTRACE_REQUEST_FLAG(PT_VM_ENTRY, vm_entry)
PTRACE_REQUEST_FLAG(PT_FIRSTMACH, firstmach)

#ifdef PTRACE_HARDENING_SYSCTLS
int
sysctl_ptrace_hardening_status(SYSCTL_HANDLER_ARGS)
{
	struct prison *pr;
	int err, val;

	/*
	 * pax_get_prison always returns not NULL value
	 */
	pr = pax_get_prison(req->td->td_proc);

	val = pr->pr_hardening.hr_ptrace_hardening_status;
	err = sysctl_handle_int(oidp, &val, sizeof(int), req);
	if (err || (req->newptr == NULL))
		return (err);

	switch (val) {
	case    PAX_FEATURE_SIMPLE_DISABLED:
	case    PAX_FEATURE_SIMPLE_ENABLED:
		if (pr == &prison0)
			ptrace_hardening_status = val;

		prison_lock(pr);
		pr->pr_hardening.hr_ptrace_hardening_status = val;
		prison_unlock(pr);
		break;
	default:
		return (EINVAL);
	}

	return (0);
}

int
sysctl_ptrace_hardening_flag(SYSCTL_HANDLER_ARGS)
{
	struct prison *pr;
	int err, val;

	pr = pax_get_prison(req->td->td_proc);

	val = pr->pr_hardening.hr_ptrace_hardening_flag_status;
	err = sysctl_handle_int(oidp, &val, sizeof(int), req);
	if (err || (req->newptr == NULL))
		return (err);

	switch (val) {
	case PAX_FEATURE_SIMPLE_DISABLED:
	case PAX_FEATURE_SIMPLE_ENABLED:
		if (pr == &prison0)
			ptrace_hardening_flag_status = val;

		prison_lock(pr);
		pr->pr_hardening.hr_ptrace_hardening_flag_status = val;
		prison_unlock(pr);
		break;
	default:
		return (EINVAL);
	}

	return (0);
}

int
sysctl_ptrace_hardening_flagall(SYSCTL_HANDLER_ARGS)
{
	struct prison *pr;
	struct sysctl_oid_list *oidlist;
	struct sysctl_oid *oid;
	size_t buflen, sysctlpreflen;
	int err, val;

	pr = pax_get_prison(req->td->td_proc);

	oidlist = &sysctl___hardening_ptrace_flag.oid_children;
	sysctlpreflen = sizeof("hardening.ptrace.flag.");

	val = pr->pr_hardening.hr_ptrace_request_flags_all;
	err = sysctl_handle_int(oidp, &val, sizeof(int), req);
	if (err || (req->newptr == NULL))
		return (err);

	switch (val) {
	case PAX_FEATURE_SIMPLE_DISABLED:
	case PAX_FEATURE_SIMPLE_ENABLED:
		if (pr == &prison0)
			ptrace_request_flags_all = val;

		prison_lock(pr);
		pr->pr_hardening.hr_ptrace_request_flags_all = val;
		prison_unlock(pr);

		mtx_lock(&ptrace_request_flags_mtx);

		SLIST_FOREACH(oid, oidlist, oid_link) {
			buflen = sysctlpreflen + strlen(oid->oid_name);
			char *buf = malloc(sizeof(char) * (buflen + 1), 
				HARDENING_PTRACE, M_WAITOK);
			snprintf(buf, buflen, "hardening.ptrace.flag.%s",
				oid->oid_name);	
			buf[buflen] = '\0';

			kernel_sysctlbyname(req->td, buf, NULL, 
				0, &val, sizeof(val), NULL, 0);
			free(buf, HARDENING_PTRACE);
		}

		mtx_unlock(&ptrace_request_flags_mtx);
		break;
	default:
		return (EINVAL);
	}

	return (0);
}

#ifdef PTRACE_HARDENING_GRP
int
sysctl_ptrace_hardening_gid(SYSCTL_HANDLER_ARGS)
{
	struct prison *pr;
	long val;
	int err;

	pr = pax_get_prison(req->td->td_proc);

	val = pr->pr_hardening.hr_ptrace_hardening_allowed_gid;
	err = sysctl_handle_long(oidp, &val, sizeof(long), req);
	if (err || (req->newptr == NULL))
		return (err);

	if (val < 0 || val > GID_MAX)
		return (EINVAL);

	if (pr == &prison0)
		ptrace_hardening_allowed_gid = val;

	prison_lock(pr);
	pr->pr_hardening.hr_ptrace_hardening_allowed_gid = val;
	prison_unlock(pr);

	return (0);
}
#endif	/* PTRACE_HARDENING_GRP */
#endif	/* PTRACE_HARDENING_SYSCTLS */

int
ptrace_hardening(struct thread *td, struct proc *p, int ptrace_flag)
{
	struct prison *pr;
	uid_t uid;
	gid_t gid;
	pid_t pid;

	pr = pax_get_prison(td->td_proc);

	if (pr->pr_hardening.hr_ptrace_hardening_status ==
	    PAX_FEATURE_SIMPLE_DISABLED)
		return (0);

	/*
	 * XXXOP
	 * td->td_proc instead of p?
	 */
	if (p->p_ptrace_hardening & PTRACE_HARDENING_MODE_PUBLIC)
		return (0);

	uid = td->td_ucred->cr_ruid;
	gid = td->td_ucred->cr_rgid;
	pid = p->p_pid;

#ifndef	PTRACE_HARDENING_NOROOT
	if (uid == 0)
		return (0);
#endif

	if ((pr->pr_hardening.hr_ptrace_hardening_flag_status ==
	    PAX_FEATURE_SIMPLE_ENABLED) &&
	    (pr->pr_hardening.hr_ptrace_request_flags[ptrace_flag] ==
	    PAX_FEATURE_SIMPLE_DISABLED))
		goto fail;

#ifdef PTRACE_HARDENING_GRP
	if ((pr->pr_hardening.hr_ptrace_hardening_allowed_gid != 0) &&
	    (gid != pr->pr_hardening.hr_ptrace_hardening_allowed_gid))
		goto fail;
#endif	/* PTRACE_HARDENING_GRP */

	return (0);

fail:
	ptrace_log_hardening(td->td_proc, __func__, "forbidden ptrace call attempt "
	    "from %ld:%ld, pid %ld\n", uid, gid, pid);

	return (EPERM);
}

// XXXOP
static void
ptrace_hardening_sysinit(void)
{
	int mtx_flag;
	size_t i;

	if (ptrace_hardening_status < 0 || ptrace_hardening_status > 1)
		ptrace_hardening_status = PTRACE_HARDENING_ENABLED;

	if (ptrace_hardening_flag_status < 0 || ptrace_hardening_flag_status > 1)
		ptrace_hardening_flag_status = PTRACE_HARDENING_REQFLAG_ENABLED;

	for (i = 0; i < sizeof(ptrace_request_flags); i++)
		if (ptrace_request_flags[i] < 0 || ptrace_request_flags[i] > 1)
			ptrace_request_flags[i] = 0;

	mtx_flag = MTX_DEF;

#ifndef PTRACE_HARDENING_DEBUG
	mtx_flag |= MTX_NOWITNESS;
#endif

	printf("[PTRACE HARDENING] status : %s\n",
	    ptrace_hardening_status ? "enabled" : "disabled");
	printf("[PTRACE HARDENING] flags : %s\n",
	    ptrace_hardening_flag_status ? "enabled" : "disabled");

#ifdef PTRACE_HARDENING_GRP
	printf("[PTRACE HARDENING] allowed gid : %d\n",
	    ptrace_hardening_allowed_gid);
#endif

	mtx_init(&ptrace_request_flags_mtx, "Ptrace request flags mutex",
		NULL, mtx_flag);
}
SYSINIT(ptrace, SI_SUB_PTRACE_HARDENING, SI_ORDER_FIRST, ptrace_hardening_sysinit, NULL);

static void
ptrace_hardening_sysuninit(void)
{
	mtx_destroy(&ptrace_request_flags_mtx);
}
SYSUNINIT(ptrace, SI_SUB_PTRACE_HARDENING, SI_ORDER_FIRST,
	ptrace_hardening_sysuninit, NULL);

void
ptrace_hardening_init_prison(struct prison *pr)
{
	if (pr == NULL)
		return;

	if (pr->pr_hardening.hr_ptrace_hardening_set)
		return;

	prison_lock(pr);

	pr->pr_hardening.hr_ptrace_hardening_status = ptrace_hardening_status;

#ifdef PTRACE_HARDENING_GRP
	pr->pr_hardening.hr_ptrace_hardening_allowed_gid =
		ptrace_hardening_allowed_gid;
#endif

	pr->pr_hardening.hr_ptrace_hardening_flag_status =
		ptrace_hardening_flag_status;
	pr->pr_hardening.hr_ptrace_request_flags_all =
		ptrace_request_flags_all;
	memcpy(pr->pr_hardening.hr_ptrace_request_flags, ptrace_request_flags,
		sizeof(pr->pr_hardening.hr_ptrace_request_flags));

	pr->pr_hardening.hr_ptrace_hardening_set = 1;

	prison_unlock(pr);
}
