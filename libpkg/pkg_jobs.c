/*-
 * Copyright (c) 2011-2012 Baptiste Daroussin <bapt@FreeBSD.org>
 * Copyright (c) 2011-2012 Julien Laffaye <jlaffaye@FreeBSD.org>
 * Copyright (c) 2011-2012 Marin Atanasov Nikolov <dnaeon@gmail.com>
 * Copyright (c) 2013 Matthew Seaman <matthew@FreeBSD.org>
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/mount.h>

#include <assert.h>
#include <errno.h>
#include <libutil.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "pkg.h"
#include "private/event.h"
#include "private/pkg.h"
#include "private/pkgdb.h"

static int get_remote_pkg(struct pkg_jobs *j, const char *pattern, match_t m, bool root);
static struct pkg * get_local_pkg(struct pkg_jobs *j, const char *origin);
static int pkg_jobs_fetch(struct pkg_jobs *j);
static bool newer_than_local_pkg(struct pkg_jobs *j, struct pkg *rp, bool force);
static bool new_pkg_version(struct pkg_jobs *j);
static int order_pool(struct pkg_jobs *j);

int
pkg_jobs_new(struct pkg_jobs **j, pkg_jobs_t t, struct pkgdb *db)
{
	assert(db != NULL);
	assert(t != PKG_JOBS_INSTALL || db->type == PKGDB_REMOTE);

	if ((*j = calloc(1, sizeof(struct pkg_jobs))) == NULL) {
		pkg_emit_errno("calloc", "pkg_jobs");
		return (EPKG_FATAL);
	}

	(*j)->db = db;
	(*j)->type = t;
	(*j)->solved = false;
	(*j)->flags = PKG_FLAG_NONE;

	return (EPKG_OK);
}

void
pkg_jobs_set_flags(struct pkg_jobs *j, pkg_flags flags)
{
	j->flags = flags;
}

int
pkg_jobs_set_repository(struct pkg_jobs *j, const char *name)
{
	/* TODO should validate if the repository exists */
	j->reponame = name;

	return (EPKG_OK);
}

void
pkg_jobs_free(struct pkg_jobs *j)
{
	if (j == NULL)
		return;

	if ((j->flags & PKG_FLAG_DRY_RUN) == 0)
		pkgdb_release_lock(j->db);

	HASH_FREE(j->jobs, pkg, pkg_free);
	LL_FREE(j->patterns, job_pattern, free);

	free(j);
}

int
pkg_jobs_add(struct pkg_jobs *j, match_t match, char **argv, int argc)
{
	struct job_pattern *jp;
	int i = 0;

	if (j->solved) {
		pkg_emit_error("The job has already been solved. "
		    "Impossible to append new elements");
		return (EPKG_FATAL);
	}

	for (i = 0; i < argc; i++) {
		jp = malloc(sizeof(struct job_pattern));
		jp->pattern = argv + i;
		jp->nb = 1;
		jp->match = match;
		LL_APPEND(j->patterns, jp);
	}

	return (EPKG_OK);
}

static int
jobs_solve_deinstall(struct pkg_jobs *j)
{
	struct job_pattern *jp = NULL;
	struct pkg *pkg = NULL;
	struct pkgdb_it *it;
	char *origin;
	bool recursive = false;

	if ((j->flags & PKG_FLAG_RECURSIVE) == PKG_FLAG_RECURSIVE)
		recursive = true;

	LL_FOREACH(j->patterns, jp) {
		if ((it = pkgdb_query_delete(j->db, jp->match, jp->nb,
		    jp->pattern, recursive)) == NULL)
			return (EPKG_FATAL);

		while (pkgdb_it_next(it, &pkg, PKG_LOAD_BASIC) == EPKG_OK) {
			pkg_get(pkg, PKG_ORIGIN, &origin);
			HASH_ADD_KEYPTR(hh, j->jobs, origin, strlen(origin), pkg);
			pkg = NULL;
		}
		pkgdb_it_free(it);
	}
	j->solved = true;

	return( EPKG_OK);
}

static bool
recursive_autoremove(struct pkg_jobs *j)
{
	struct pkg *pkg1, *tmp1, *pkg2, *tmp2;
	struct pkg_dep *dep;
	char *origin;

	HASH_ITER(hh, j->bulk, pkg1, tmp1) {
		if (HASH_COUNT(pkg1->rdeps) == 0) {
			HASH_DEL(j->bulk, pkg1);
			pkg_get(pkg1, PKG_ORIGIN, &origin);
			HASH_ADD_KEYPTR(hh, j->jobs, origin, strlen(origin), pkg1);
			HASH_ITER(hh, j->bulk, pkg2, tmp2) {
				HASH_FIND_STR(pkg2->rdeps, origin, dep);
				if (dep != NULL) {
					HASH_DEL(pkg2->rdeps, dep);
					pkg_dep_free(dep);
				}
			}
			return (true);
		}
	}

	return (false);
}

static int
jobs_solve_autoremove(struct pkg_jobs *j)
{
	struct pkg *pkg = NULL;
	struct pkgdb_it *it;
	char *origin;

	if ((it = pkgdb_query(j->db, " WHERE automatic=1 ", MATCH_CONDITION)) == NULL)
		return (EPKG_FATAL);

	while (pkgdb_it_next(it, &pkg, PKG_LOAD_BASIC|PKG_LOAD_RDEPS) == EPKG_OK) {
		pkg_get(pkg, PKG_ORIGIN, &origin);
		HASH_ADD_KEYPTR(hh, j->bulk, origin, strlen(origin), pkg);
		pkg = NULL;
	}
	pkgdb_it_free(it);

	while (recursive_autoremove(j));

	HASH_FREE(j->bulk, pkg, pkg_free);

	j->solved = true;

	return (EPKG_OK);
}

static int
jobs_solve_upgrade(struct pkg_jobs *j)
{
	struct pkg *pkg = NULL;
	struct pkgdb_it *it;
	char *origin;
	bool pkgversiontest = false;

	if ((j->flags & PKG_FLAG_PKG_VERSION_TEST) != PKG_FLAG_PKG_VERSION_TEST)
		if (new_pkg_version(j)) {
			pkg_emit_newpkgversion();
			goto order;
		}

	if ((j->flags & PKG_FLAG_PKG_VERSION_TEST) != 0)
		pkgversiontest = true;

	if ((it = pkgdb_query(j->db, NULL, MATCH_ALL)) == NULL)
		return (EPKG_FATAL);

	while (pkgdb_it_next(it, &pkg, PKG_LOAD_BASIC) == EPKG_OK) {
		pkg_get(pkg, PKG_ORIGIN, &origin);
		/* Do not test we ignore what doesn't exists remotely */
		get_remote_pkg(j, origin, MATCH_EXACT, false);
		pkg = NULL;
	}
	pkgdb_it_free(it);

order:
	HASH_FREE(j->seen, pkg, pkg_free);

	/* now order the pool */
	while (HASH_COUNT(j->bulk) > 0) {
		if (order_pool(j) != EPKG_OK)
			return (EPKG_FATAL);
	}

	j->solved = true;

	return (EPKG_OK);
}

static void
remove_from_deps(struct pkg_jobs *j, const char *origin)
{
	struct pkg *pkg, *tmp;
	struct pkg_dep *d;

	HASH_ITER(hh, j->bulk, pkg, tmp) {
		HASH_FIND_STR(pkg->deps, __DECONST(char *, origin), d);
		if (d != NULL) {
			HASH_DEL(pkg->deps, d);
			pkg_dep_free(d);
		}
	}
}

static int
order_pool(struct pkg_jobs *j)
{
	struct pkg *pkg, *tmp;
	char *origin;
	unsigned int nb;

	nb = HASH_COUNT(j->bulk);
	HASH_ITER(hh, j->bulk, pkg, tmp) {
		pkg_get(pkg, PKG_ORIGIN, &origin);
		if (HASH_COUNT(pkg->deps) == 0) {
			HASH_DEL(j->bulk, pkg);
			HASH_ADD_KEYPTR(hh, j->jobs, origin, strlen(origin), pkg);
			remove_from_deps(j, origin);
		}
	}

	if (nb == HASH_COUNT(j->bulk)) {
		pkg_emit_error("Error while ordering the jobs, probably a circular dependency");
		return (EPKG_FATAL);
	}

	return (EPKG_OK);
}

static int
populate_rdeps(struct pkg_jobs *j, struct pkg *p)
{
	struct pkg *pkg;
	struct pkg_dep *d = NULL;

	while (pkg_rdeps(p, &d) == EPKG_OK) {
		HASH_FIND_STR(j->bulk, __DECONST(char *, pkg_dep_get(d, PKG_DEP_ORIGIN)), pkg);
		if (pkg != NULL)
			continue;
		HASH_FIND_STR(j->seen, __DECONST(char *, pkg_dep_get(d, PKG_DEP_ORIGIN)), pkg);
		if (pkg != NULL)
			continue;
		if (get_remote_pkg(j, pkg_dep_get(d, PKG_DEP_ORIGIN), MATCH_EXACT, true) != EPKG_OK) {
			pkg_emit_error("Missing dependency matching '%s'", pkg_dep_get(d, PKG_DEP_ORIGIN));
			return (EPKG_FATAL);
		}
	}

	return (EPKG_OK);
}

static int
populate_deps(struct pkg_jobs *j, struct pkg *p)
{
	struct pkg *pkg;
	struct pkg_dep *d = NULL;

	while (pkg_deps(p, &d) == EPKG_OK) {
		HASH_FIND_STR(j->bulk, __DECONST(char *, pkg_dep_get(d, PKG_DEP_ORIGIN)), pkg);
		if (pkg != NULL)
			continue;
		HASH_FIND_STR(j->seen, __DECONST(char *, pkg_dep_get(d, PKG_DEP_ORIGIN)), pkg);
		if (pkg != NULL)
			continue;
		if (get_remote_pkg(j, pkg_dep_get(d, PKG_DEP_ORIGIN), MATCH_EXACT, false) != EPKG_OK) {
			pkg_emit_error("Missing dependency matching '%s'", pkg_dep_get(d, PKG_DEP_ORIGIN));
			return (EPKG_FATAL);
		}
	}

	return (EPKG_OK);
}

static bool
new_pkg_version(struct pkg_jobs *j)
{
	struct pkg *p;
	const char *origin = "ports-mgmt/pkg";

	/* determine local pkgng */
	p = get_local_pkg(j, origin);

	if (p == NULL) {
		origin = "ports-mgmt/pkg-devel";
		p = get_local_pkg(j, origin);
	}

	/* you are using git version skip */
	if (p == NULL)
		return (false);

	if (get_remote_pkg(j, origin, MATCH_EXACT, true) == EPKG_OK)
		return (true);

	return (false);
}

static int
get_remote_pkg(struct pkg_jobs *j, const char *pattern, match_t m, bool root)
{
	struct pkg *p = NULL;
	struct pkg *p1;
	struct pkgdb_it *it;
	char *origin;
	const char *buf1, *buf2;
	bool force = false;
	int rc = EPKG_FATAL;
	unsigned flags = PKG_LOAD_BASIC|PKG_LOAD_OPTIONS|PKG_LOAD_SHLIBS_REQUIRED;

	if (root && (j->flags & PKG_FLAG_FORCE) == PKG_FLAG_FORCE)
		force = true;

	if (j->type == PKG_JOBS_UPGRADE && (j->flags & PKG_FLAG_FORCE) == PKG_FLAG_FORCE)
		force = true;

	if (j->type == PKG_JOBS_FETCH) {
		if ((j->flags & PKG_FLAG_WITH_DEPS) == PKG_FLAG_WITH_DEPS)
			flags |= PKG_LOAD_DEPS;
		if ((j->flags & PKG_FLAG_UPGRADES_FOR_INSTALLED) == PKG_FLAG_UPGRADES_FOR_INSTALLED)
			flags |= PKG_LOAD_DEPS;
	} else {
		flags |= PKG_LOAD_DEPS;
	}

	if (root && (j->flags & PKG_FLAG_RECURSIVE) == PKG_FLAG_RECURSIVE)
		flags |= PKG_LOAD_RDEPS;

	if ((it = pkgdb_rquery(j->db, pattern, m, j->reponame)) == NULL)
		return (rc);

	while (pkgdb_it_next(it, &p, flags) == EPKG_OK) {
		pkg_get(p, PKG_ORIGIN, &origin);
		HASH_FIND_STR(j->bulk, origin, p1);
		if (p1 != NULL) {
			pkg_get(p1, PKG_VERSION, &buf1);
			pkg_get(p, PKG_VERSION, &buf2);
			p->direct = root;
			if (pkg_version_cmp(buf1, buf2) != 1)
				continue;
			HASH_DEL(j->bulk, p1);
			pkg_free(p1);
		}

		if (j->type != PKG_JOBS_FETCH) {
			if (!newer_than_local_pkg(j, p, force)) {
				if (root)
					pkg_emit_already_installed(p);
				rc = EPKG_OK;
				HASH_ADD_KEYPTR(hh, j->seen, origin, strlen(origin), p);
				continue;
			}
		}

		rc = EPKG_OK;
		p->direct = root;
		HASH_ADD_KEYPTR(hh, j->bulk, origin, strlen(origin), p);
		if (populate_deps(j, p) == EPKG_FATAL) {
			rc = EPKG_FATAL;
			break;
		}

		if (populate_rdeps(j, p) == EPKG_FATAL) {
			rc = EPKG_FATAL;
			break;
		}
		p = NULL;
	}

	pkgdb_it_free(it);

	return (rc);
}

static struct pkg *
get_local_pkg(struct pkg_jobs *j, const char *origin)
{
	struct pkg *pkg = NULL;
	struct pkgdb_it *it;

	if ((it = pkgdb_query(j->db, origin, MATCH_EXACT)) == NULL)
		return (NULL);

	if (pkgdb_it_next(it, &pkg, PKG_LOAD_BASIC|PKG_LOAD_DEPS|PKG_LOAD_OPTIONS|PKG_LOAD_SHLIBS_REQUIRED) != EPKG_OK)
		pkg = NULL;

	pkgdb_it_free(it);

	return (pkg);
}

static bool
newer_than_local_pkg(struct pkg_jobs *j, struct pkg *rp, bool force)
{
	char *origin, *newversion, *oldversion, *oldsize, *newsize;
	struct pkg *lp;
	struct sbuf *sb1, *sb2;
	struct pkg_option *o = NULL;
	struct pkg_dep *d = NULL;
	struct pkg_shlib *s = NULL;
	bool automatic, locked;
	int cmp = 0;

	pkg_get(rp, PKG_ORIGIN, &origin);
	lp = get_local_pkg(j, origin);

	/* obviously yes because local doesn't exists */
	if (lp == NULL) {
		pkg_set(rp, PKG_AUTOMATIC, (int64_t)true);
		return (true);
	}

	pkg_get(lp, PKG_LOCKED, &locked,
	    PKG_AUTOMATIC, &automatic,
	    PKG_VERSION, &oldversion,
	    PKG_FLATSIZE, &oldsize);

	if (locked) {
		pkg_free(lp);
		return (false);
	}

	pkg_get(rp, PKG_VERSION, &newversion, PKG_FLATSIZE, &newsize);
	pkg_set(rp, PKG_VERSION, oldversion, PKG_NEWVERSION, newversion,
	    PKG_NEW_FLATSIZE, newsize, PKG_FLATSIZE, oldsize,
	    PKG_AUTOMATIC, (int64_t)automatic);

	if (force) {
		pkg_free(lp);
		return (true);
	}

	/* compare versions */
	cmp = pkg_version_cmp(newversion, oldversion);

	if (cmp == 1) {
		pkg_free(lp);
		return (true);
	}

	if (cmp == 0) {
		pkg_free(lp);
		return (false);
	}

	/* compare options */
	sb1 = sbuf_new_auto();
	sb2 = sbuf_new_auto();

	while (pkg_options(rp, &o) == EPKG_OK)
		sbuf_printf(sb1, "%s=%s ", pkg_option_opt(o), pkg_option_value(o));

	o = NULL;
	while (pkg_options(lp, &o) == EPKG_OK)
		sbuf_printf(sb2, "%s=%s ", pkg_option_opt(o), pkg_option_value(o));

	sbuf_finish(sb1);
	sbuf_finish(sb2);

	if (strcmp(sbuf_data(sb1), sbuf_data(sb2)) != 0) {
		sbuf_delete(sb1);
		sbuf_delete(sb2);
		pkg_free(lp);
		return (true);
	}

	/* What about the direct deps */
	sbuf_reset(sb1);
	sbuf_reset(sb2);

	while (pkg_deps(rp, &d) == EPKG_OK)
		sbuf_cat(sb1, pkg_dep_get(d, PKG_DEP_NAME));

	d = NULL;
	while (pkg_deps(lp, &d) == EPKG_OK)
		sbuf_cat(sb2, pkg_dep_get(d, PKG_DEP_NAME));

	sbuf_finish(sb1);
	sbuf_finish(sb2);

	if (strcmp(sbuf_data(sb1), sbuf_data(sb2)) != 0) {
		sbuf_delete(sb1);
		sbuf_delete(sb2);
		pkg_free(lp);

		return (true);
	}

	/* Finish by the shlibs */
	sbuf_reset(sb1);
	sbuf_reset(sb2);

	while (pkg_shlibs_required(rp, &s) == EPKG_OK)
		sbuf_cat(sb1, pkg_shlib_name(s));

	d = NULL;
	while (pkg_shlibs_required(lp, &s) == EPKG_OK)
		sbuf_cat(sb2, pkg_shlib_name(s));

	sbuf_finish(sb1);
	sbuf_finish(sb2);

	if (strcmp(sbuf_data(sb1), sbuf_data(sb2)) != 0) {
		sbuf_delete(sb1);
		sbuf_delete(sb2);
		pkg_free(lp);

		return (true);
	}

	sbuf_delete(sb1);
	sbuf_delete(sb2);

	return (false);
}

static int
jobs_solve_install(struct pkg_jobs *j)
{
	struct job_pattern *jp = NULL;
	struct pkg *pkg, *tmp, *p;
	struct pkg_dep *d;

	if ((j->flags & PKG_FLAG_PKG_VERSION_TEST) != PKG_FLAG_PKG_VERSION_TEST)
		if (new_pkg_version(j)) {
			pkg_emit_newpkgversion();
			goto order;
		}

	LL_FOREACH(j->patterns, jp) {
		if (get_remote_pkg(j, jp->pattern[0], jp->match, true) == EPKG_FATAL)
			pkg_emit_error("No packages matching '%s' has been found in the repositories", jp->pattern[0]);
	}

	if (HASH_COUNT(j->bulk) == 0)
		return (EPKG_OK);

	/* remove everything seen from deps */
	HASH_ITER(hh, j->bulk, pkg, tmp) {
		d = NULL;
		while (pkg_deps(pkg, &d) == EPKG_OK) {
			HASH_FIND_STR(j->seen, __DECONST(char *, pkg_dep_get(d, PKG_DEP_ORIGIN)), p);
			if (p != NULL) {
				HASH_DEL(pkg->deps, d);
				pkg_dep_free(d);
			}
		}
		if (pkg->direct) {
			if ((j->flags & PKG_FLAG_AUTOMATIC) == PKG_FLAG_AUTOMATIC)
				pkg_set(pkg, PKG_AUTOMATIC, (int64_t)true);
			else
				pkg_set(pkg, PKG_AUTOMATIC, (int64_t)false);
		}
	}

order:
	HASH_FREE(j->seen, pkg, pkg_free);

	/* now order the pool */
	while (HASH_COUNT(j->bulk) > 0) {
		if (order_pool(j) != EPKG_OK)
			return (EPKG_FATAL);
	}

	j->solved = true;

	return (EPKG_OK);
}

static int
jobs_solve_fetch(struct pkg_jobs *j)
{
	struct job_pattern *jp = NULL;
	struct pkg *pkg = NULL;
	struct pkgdb_it *it;
	char *origin;
	unsigned flag = PKG_LOAD_BASIC;

	if ((j->flags & PKG_FLAG_WITH_DEPS) != 0)
		flag |= PKG_LOAD_DEPS;

	if ((j->flags & PKG_FLAG_UPGRADES_FOR_INSTALLED) == PKG_FLAG_UPGRADES_FOR_INSTALLED) {
		if ((it = pkgdb_query(j->db, NULL, MATCH_ALL)) == NULL)
			return (EPKG_FATAL);

		while (pkgdb_it_next(it, &pkg, PKG_LOAD_BASIC) == EPKG_OK) {
			pkg_get(pkg, PKG_ORIGIN, &origin);
			/* Do not test we ignore what doesn't exists remotely */
			get_remote_pkg(j, origin, MATCH_EXACT, false);
			pkg = NULL;
		}
		pkgdb_it_free(it);
	} else {
		LL_FOREACH(j->patterns, jp) {
			if (get_remote_pkg(j, jp->pattern[0], jp->match, true) == EPKG_FATAL)
				pkg_emit_error("No packages matching '%s' has been found in the repositories", jp->pattern[0]);
		}
	}

	HASH_FREE(j->seen, pkg, pkg_free);
	/* No need to order we are just fetching */
	j->jobs = j->bulk;

	j->solved = true;

	return (EPKG_OK);
}

int
pkg_jobs_solve(struct pkg_jobs *j)
{
	bool dry_run = false;

	if ((j->flags & PKG_FLAG_DRY_RUN) == PKG_FLAG_DRY_RUN)
		dry_run = true;

	if (!dry_run && pkgdb_obtain_lock(j->db) != EPKG_OK)
		return (EPKG_FATAL);


	switch (j->type) {
	case PKG_JOBS_AUTOREMOVE:
		return (jobs_solve_autoremove(j));
	case PKG_JOBS_DEINSTALL:
		return (jobs_solve_deinstall(j));
	case PKG_JOBS_UPGRADE:
		return (jobs_solve_upgrade(j));
	case PKG_JOBS_INSTALL:
		return (jobs_solve_install(j));
	case PKG_JOBS_FETCH:
		return (jobs_solve_fetch(j));
	default:
		return (EPKG_FATAL);
	}
}

int
pkg_jobs_find(struct pkg_jobs *j, const char *origin, struct pkg **p)
{
	struct pkg *pkg;

	HASH_FIND_STR(j->jobs, __DECONST(char *, origin), pkg);
	if (pkg == NULL)
		return (EPKG_FATAL);

	if (p != NULL)
		*p = pkg;

	return (EPKG_OK);
}

int
pkg_jobs_count(struct pkg_jobs *j)
{
	assert(j != NULL);

	return (HASH_COUNT(j->jobs));
}

pkg_jobs_t
pkg_jobs_type(struct pkg_jobs *j)
{
	return (j->type);
}

int
pkg_jobs(struct pkg_jobs *j, struct pkg **pkg)
{
	assert(j != NULL);

	HASH_NEXT(j->jobs, (*pkg));
}

static int
pkg_jobs_keep_files_to_del(struct pkg *p1, struct pkg *p2)
{
	struct pkg_file *f = NULL;
	struct pkg_dir *d = NULL;

	while (pkg_files(p1, &f) == EPKG_OK) {
		if (f->keep)
			continue;

		f->keep = pkg_has_file(p2, pkg_file_path(f));
	}

	while (pkg_dirs(p1, &d) == EPKG_OK) {
		if (d->keep)
			continue;

		d->keep = pkg_has_dir(p2, pkg_dir_path(d));
	}

	return (EPKG_OK);
}

static int
pkg_jobs_install(struct pkg_jobs *j)
{
	struct pkg *p = NULL;
	struct pkg *pkg = NULL;
	struct pkg *newpkg = NULL;
	struct pkg *pkg_temp = NULL;
	struct pkgdb_it *it = NULL;
	struct pkg *pkg_queue = NULL;
	struct pkg_manifest_key *keys = NULL;
	char path[MAXPATHLEN + 1];
	const char *cachedir = NULL;
	int flags = 0;
	int retcode = EPKG_FATAL;
	int lflags = PKG_LOAD_BASIC | PKG_LOAD_FILES | PKG_LOAD_SCRIPTS |
	    PKG_LOAD_DIRS;
	bool handle_rc = false;

	/* Fetch */
	if (pkg_jobs_fetch(j) != EPKG_OK)
		return (EPKG_FATAL);

	if (j->flags & PKG_FLAG_SKIP_INSTALL)
		return (EPKG_OK);

	if (pkg_config_string(PKG_CONFIG_CACHEDIR, &cachedir) != EPKG_OK)
		return (EPKG_FATAL);
	
	pkg_config_bool(PKG_CONFIG_HANDLE_RC_SCRIPTS, &handle_rc);

	p = NULL;
	pkg_manifest_keys_new(&keys);
	/* Install */
	pkgdb_transaction_begin(j->db->sqlite, "upgrade");

	while (pkg_jobs(j, &p) == EPKG_OK) {
		const char *pkgorigin, *pkgrepopath, *newversion, *origin;
		bool automatic, locked;
		flags = 0;

		pkg_get(p, PKG_ORIGIN, &pkgorigin, PKG_REPOPATH, &pkgrepopath,
		    PKG_NEWVERSION, &newversion, PKG_AUTOMATIC, &automatic);

		if (newversion != NULL) {
			pkg = NULL;
			it = pkgdb_query(j->db, pkgorigin, MATCH_EXACT);
			if (it != NULL) {
				if (pkgdb_it_next(it, &pkg, lflags) == EPKG_OK) {
					pkg_get(pkg, PKG_LOCKED, &locked);
					if (locked) {
						pkg_emit_locked(pkg);
						pkgdb_it_free(it);
						retcode = EPKG_LOCKED;
						pkgdb_transaction_rollback(j->db->sqlite, "upgrade");
						goto cleanup; /* Bail out */
					}

					LL_APPEND(pkg_queue, pkg);
					if ((j->flags & PKG_FLAG_NOSCRIPT) == 0)
						pkg_script_run(pkg,
						    PKG_SCRIPT_PRE_DEINSTALL);
					pkg_get(pkg, PKG_ORIGIN, &origin);
					/*
					 * stop the different related services
					 * if the user wants that and the
					 * service is running
					 */
					if (handle_rc)
						pkg_start_stop_rc_scripts(pkg,
						    PKG_RC_STOP);
					pkgdb_unregister_pkg(j->db, origin);
					pkg = NULL;
				}
				pkgdb_it_free(it);
			}
		}

		it = pkgdb_integrity_conflict_local(j->db, pkgorigin);

		if (it != NULL) {
			pkg = NULL;
			while (pkgdb_it_next(it, &pkg, lflags) == EPKG_OK) {

				pkg_get(pkg, PKG_LOCKED, &locked);
				if (locked) {
					pkg_emit_locked(pkg);
					pkgdb_it_free(it);
					retcode = EPKG_LOCKED;
					pkgdb_transaction_rollback(j->db->sqlite, "upgrade");
					goto cleanup; /* Bail out */
				}

				LL_APPEND(pkg_queue, pkg);
				if ((j->flags & PKG_FLAG_NOSCRIPT) == 0)
					pkg_script_run(pkg,
					    PKG_SCRIPT_PRE_DEINSTALL);
				pkg_get(pkg, PKG_ORIGIN, &origin);
				/*
				 * stop the different related services if the
				 * user wants that and the service is running
				 */
				if (handle_rc)
					pkg_start_stop_rc_scripts(pkg,
					    PKG_RC_STOP);
				pkgdb_unregister_pkg(j->db, origin);
				pkg = NULL;
			}
			pkgdb_it_free(it);
		}
		snprintf(path, sizeof(path), "%s/%s", cachedir, pkgrepopath);

		pkg_open(&newpkg, path, keys, 0);
		if (newversion != NULL) {
			pkg_emit_upgrade_begin(p);
		} else {
			pkg_emit_install_begin(newpkg);
		}
		LL_FOREACH(pkg_queue, pkg)
			pkg_jobs_keep_files_to_del(pkg, newpkg);

		LL_FOREACH_SAFE(pkg_queue, pkg, pkg_temp) {
			pkg_get(pkg, PKG_ORIGIN, &origin);
			if (strcmp(pkgorigin, origin) == 0) {
				LL_DELETE(pkg_queue, pkg);
				pkg_delete_files(pkg, 1);
				if ((j->flags & PKG_FLAG_NOSCRIPT) == 0)
					pkg_script_run(pkg,
					    PKG_SCRIPT_POST_DEINSTALL);
				pkg_delete_dirs(j->db, pkg, 0);
				pkg_free(pkg);
				break;
			}
		}

		if ((j->flags & PKG_FLAG_FORCE) != 0)
			flags |= PKG_ADD_FORCE | PKG_FLAG_FORCE;
		if ((j->flags & PKG_FLAG_NOSCRIPT) != 0)
			flags |= PKG_ADD_NOSCRIPT;
		flags |= PKG_ADD_UPGRADE;
		if (automatic)
			flags |= PKG_ADD_AUTOMATIC;

		if (pkg_add(j->db, path, flags, keys) != EPKG_OK) {
			pkgdb_transaction_rollback(j->db->sqlite, "upgrade");
			goto cleanup;
		}

		if (newversion != NULL)
			pkg_emit_upgrade_finished(p);
		else
			pkg_emit_install_finished(newpkg);

		if (pkg_queue == NULL) {
			pkgdb_transaction_commit(j->db->sqlite, "upgrade");
			pkgdb_transaction_begin(j->db->sqlite, "upgrade");
		}
	}

	retcode = EPKG_OK;

	cleanup:
	pkgdb_transaction_commit(j->db->sqlite, "upgrade");
	pkg_free(newpkg);
	pkg_manifest_keys_free(keys);

	return (retcode);
}

static int
pkg_jobs_deinstall(struct pkg_jobs *j)
{
	struct pkg *p = NULL;
	int retcode;
	int flags = 0;

	if ((j->flags & PKG_FLAG_DRY_RUN) != 0)
		return (EPKG_OK); /* Do nothing */

	if ((j->flags & PKG_FLAG_FORCE) != 0)
		flags = PKG_DELETE_FORCE;

	if ((j->flags & PKG_FLAG_NOSCRIPT) != 0)
		flags |= PKG_DELETE_NOSCRIPT;

	while (pkg_jobs(j, &p) == EPKG_OK) {
		retcode = pkg_delete(p, j->db, flags);

		if (retcode != EPKG_OK)
			return (retcode);
	}

	return (EPKG_OK);
}

int
pkg_jobs_apply(struct pkg_jobs *j)
{
	int rc;

	if (!j->solved) {
		pkg_emit_error("The jobs hasn't been solved");
		return (EPKG_FATAL);
	}

	switch (j->type) {
	case PKG_JOBS_INSTALL:
		pkg_plugins_hook_run(PKG_PLUGIN_HOOK_PRE_INSTALL, j, j->db);
		rc = pkg_jobs_install(j);
		pkg_plugins_hook_run(PKG_PLUGIN_HOOK_POST_INSTALL, j, j->db);
		break;
	case PKG_JOBS_DEINSTALL:
		pkg_plugins_hook_run(PKG_PLUGIN_HOOK_PRE_DEINSTALL, j, j->db);
		rc = pkg_jobs_deinstall(j);
		pkg_plugins_hook_run(PKG_PLUGIN_HOOK_POST_DEINSTALL, j, j->db);
		break;
	case PKG_JOBS_FETCH:
		pkg_plugins_hook_run(PKG_PLUGIN_HOOK_PRE_FETCH, j, j->db);
		rc = pkg_jobs_fetch(j);
		pkg_plugins_hook_run(PKG_PLUGIN_HOOK_POST_FETCH, j, j->db);
		break;
	case PKG_JOBS_UPGRADE:
		pkg_plugins_hook_run(PKG_PLUGIN_HOOK_PRE_UPGRADE, j, j->db);
		rc = pkg_jobs_install(j);
		pkg_plugins_hook_run(PKG_PLUGIN_HOOK_POST_UPGRADE, j, j->db);
		break;
	case PKG_JOBS_AUTOREMOVE:
		pkg_plugins_hook_run(PKG_PLUGIN_HOOK_PRE_AUTOREMOVE, j, j->db);
		rc = pkg_jobs_deinstall(j);
		pkg_plugins_hook_run(PKG_PLUGIN_HOOK_POST_AUTOREMOVE, j, j->db);
		break;
	default:
		rc = EPKG_FATAL;
		pkg_emit_error("bad jobs argument");
	}

	return (rc);
}

static int
pkg_jobs_fetch(struct pkg_jobs *j)
{
	struct pkg *p = NULL;
	struct pkg *pkg = NULL;
	struct statfs fs;
	struct stat st;
	char path[MAXPATHLEN + 1];
	int64_t dlsize = 0;
	const char *cachedir = NULL;
	const char *repopath = NULL;
	char cachedpath[MAXPATHLEN];
	int ret = EPKG_OK;
	struct pkg_manifest_key *keys = NULL;
	
	if (pkg_config_string(PKG_CONFIG_CACHEDIR, &cachedir) != EPKG_OK)
		return (EPKG_FATAL);

	/* check for available size to fetch */
	while (pkg_jobs(j, &p) == EPKG_OK) {
		int64_t pkgsize;
		pkg_get(p, PKG_NEW_PKGSIZE, &pkgsize, PKG_REPOPATH, &repopath);
		snprintf(cachedpath, MAXPATHLEN, "%s/%s", cachedir, repopath);
		if (stat(cachedpath, &st) == -1)
			dlsize += pkgsize;
		else
			dlsize += pkgsize - st.st_size;
	}

	while (statfs(cachedir, &fs) == -1) {
		if (errno == ENOENT) {
			if (mkdirs(cachedir) != EPKG_OK)
				return (EPKG_FATAL);
		} else {
			pkg_emit_errno("statfs", cachedir);
			return (EPKG_FATAL);
		}
	}

	if (dlsize > ((int64_t)fs.f_bsize * (int64_t)fs.f_bfree)) {
		int64_t fsize = (int64_t)fs.f_bsize * (int64_t)fs.f_bfree;
		char dlsz[7], fsz[7];

		humanize_number(dlsz, sizeof(dlsz), dlsize, "B", HN_AUTOSCALE, 0);
		humanize_number(fsz, sizeof(fsz), fsize, "B", HN_AUTOSCALE, 0);
		pkg_emit_error("Not enough space in %s, needed %s available %s",
		    cachedir, dlsz, fsz);
		return (EPKG_FATAL);
	}

	if ((j->flags & PKG_FLAG_DRY_RUN) != 0)
		return (EPKG_OK); /* don't download anything */

	/* Fetch */
	p = NULL;
	while (pkg_jobs(j, &p) == EPKG_OK) {
		if (pkg_repo_fetch(p) != EPKG_OK)
			return (EPKG_FATAL);
	}

	p = NULL;
	/* integrity checking */
	pkg_emit_integritycheck_begin();

	pkg_manifest_keys_new(&keys);
	while (pkg_jobs(j, &p) == EPKG_OK) {
		const char *pkgrepopath;

		pkg_get(p, PKG_REPOPATH, &pkgrepopath);
		snprintf(path, sizeof(path), "%s/%s", cachedir,
		    pkgrepopath);
		if (pkg_open(&pkg, path, keys, 0) != EPKG_OK) {
			return (EPKG_FATAL);
		}

		if (pkgdb_integrity_append(j->db, pkg) != EPKG_OK)
			ret = EPKG_FATAL;
	}
	pkg_manifest_keys_free(keys);

	pkg_free(pkg);

	if (pkgdb_integrity_check(j->db) != EPKG_OK || ret != EPKG_OK)
		return (EPKG_FATAL);

	pkg_emit_integritycheck_finished();

	return (EPKG_OK);
}
