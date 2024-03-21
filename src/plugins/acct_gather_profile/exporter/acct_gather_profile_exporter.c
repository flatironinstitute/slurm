/*****************************************************************************\
 *  acct_gather_profile_exporter.c - slurm accounting profile plugin to export
 *                                   current values to files, so that they can
 *                                   be scraped.
\*****************************************************************************/

#include <sys/stat.h>
#include <fcntl.h>

#include "src/common/slurm_xlator.h"
#include "src/common/xstring.h"
#include "src/interfaces/acct_gather_profile.h"
#include "src/interfaces/gres.h"


/*
 * These variables are required by the generic plugin interface.  If they
 * are not found in the plugin, the plugin loader will ignore it.
 *
 * plugin_name - a string giving a human-readable description of the
 * plugin.  There is no maximum length, but the symbol must refer to
 * a valid string.
 *
 * plugin_type - a string suggesting the type of the plugin or its
 * applicability to a particular form of data or method of data handling.
 * If the low-level plugin API is used, the contents of this string are
 * unimportant and may be anything.  Slurm uses the higher-level plugin
 * interface which requires this string to be of the form
 *
 *	<application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "jobacct" for Slurm job completion logging) and <method>
 * is a description of how this plugin satisfies that application.  Slurm will
 * only load job completion logging plugins if the plugin_type string has a
 * prefix of "jobacct/".
 *
 * plugin_version - an unsigned 32-bit integer containing the Slurm version
 * (major.minor.micro combined into a single number).
 */
const char plugin_name[] = "AcctGatherProfile exporter plugin";
const char plugin_type[] = "acct_gather_profile/exporter";
const uint32_t plugin_version = SLURM_VERSION_NUMBER;


typedef struct {
	char *dir;
	uint32_t def;
} slurm_exporter_conf_t;

typedef struct {
	char *name;
	int taskid;
	size_t size;
	acct_gather_profile_dataset_t *dataset;
} table_t;

union data_t {
	uint64_t u;
	double	 d;
};

#define JOB_INFO_FILE_NAME "alloc"
#define GROUP_TASKS_ID 1

static slurm_exporter_conf_t exporter_conf;
static uint32_t g_profile_running = ACCT_GATHER_PROFILE_NOT_SET;
static stepd_step_rec_t *g_job = NULL;
static int g_exporter_dirfd = -1;
static char g_step_id[64];
static int g_dirfd = -1;
static bool job_info_written = false;

static table_t *tables = NULL;
static size_t tables_len = 0;



static void clear_exporter_conf()
{
	xfree(exporter_conf.dir);
	exporter_conf.dir = NULL;
	exporter_conf.def = ACCT_GATHER_PROFILE_NONE;
}


static void _free_tables(void)
{
	int i, j;

	for (i = 0; i < tables_len; i++) {
		table_t *table = &tables[i];
		for (j = 0; j < tables->size; j++)
			xfree(table->dataset[j].name);
		xfree(table->name);
		xfree(table->dataset);
	}
	xfree(tables);
	tables = NULL;
}

static uint32_t _determine_profile(void)
{
	uint32_t profile;
	xassert(g_job);

	if (g_profile_running != ACCT_GATHER_PROFILE_NOT_SET)
		profile = g_profile_running;
	else if (g_job->profile >= ACCT_GATHER_PROFILE_NONE)
		profile = g_job->profile;
	else
		profile = exporter_conf.def;

	return profile;
}


/*
 * init() is called when the plugin is loaded, before any other functions
 * are called. Put global initialization here.
 */
extern int init(void)
{
	if (!running_in_slurmstepd())
		return SLURM_SUCCESS;

	return SLURM_SUCCESS;
}

extern int fini(void)
{
	clear_exporter_conf();
	_free_tables();
	return SLURM_SUCCESS;
}

extern void acct_gather_profile_p_conf_options(s_p_options_t **full_options,
					       int *full_options_cnt)
{
	s_p_options_t options[] = {
		{"ProfileExporterDir", S_P_STRING},
		{"ProfileExporterDefault", S_P_STRING},
		{NULL} };

	transfer_s_p_options(full_options, options, full_options_cnt);
	return;
}

extern void acct_gather_profile_p_conf_set(s_p_hashtbl_t *tbl)
{
	clear_exporter_conf();

	if (tbl) {
		s_p_get_string(&exporter_conf.dir, "ProfileExporterDir", tbl);

		char *tmp = NULL;
		if (s_p_get_string(&tmp, "ProfileExporterDefault", tbl)) {
			exporter_conf.def = acct_gather_profile_from_string(tmp);
			if (exporter_conf.def == ACCT_GATHER_PROFILE_NOT_SET)
				fatal("ProfileExporterDefault can not be set to %s, please specify a valid option", tmp);
			xfree(tmp);
		}
	}

	if (!exporter_conf.dir)
		warning("No ProfileExporterDir in your acct_gather.conf file. This is required to use the %s plugin", plugin_type);

	debug("%s loaded", plugin_name);
}

extern void acct_gather_profile_p_get(enum acct_gather_profile_info info_type,
				      void *data)
{
	uint32_t *uint32 = (uint32_t *) data;
	char **tmp_char = (char **) data;

	switch (info_type) {
	case ACCT_GATHER_PROFILE_DIR:
		*tmp_char = xstrdup(exporter_conf.dir);
		break;
	case ACCT_GATHER_PROFILE_DEFAULT:
		*uint32 = exporter_conf.def;
		break;
	case ACCT_GATHER_PROFILE_RUNNING:
		*uint32 = g_profile_running;
		break;
	default:
		debug2("%s %s: info_type %d invalid", plugin_type,
		       __func__, info_type);
	}
}

static void write_job_info() {
	int infofd = openat(g_dirfd, JOB_INFO_FILE_NAME, O_WRONLY|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_TRUNC, 0644);
	if (infofd < 0) {
		warning("%s %s: %s/%s/%s: %m", plugin_type, __func__, exporter_conf.dir, g_step_id, JOB_INFO_FILE_NAME);
		return;
	}

	dprintf(infofd, "time %ld\n"
			"node %s\n"
			"user %s\n"
			"tasks %u\n"
			"cpus %u\n"
			"cpus_per_task %u\n"
			"mem %lu\n"
			, time(NULL)
			, g_job->node_name
			, g_job->user_name
			, g_job->node_tasks
			, g_job->cpus
			, g_job->cpus_per_task
			, g_job->step_mem
			);
	ListIterator i;
	gres_state_t *gres;
	if (g_job->step_gres_list) {
		i = list_iterator_create (g_job->step_gres_list);
		while ((gres = list_next (i))) {
			gres_step_state_t *data = gres->gres_data;
			dprintf(infofd, "%s %s%s%lu\n", gres->gres_name, data->type_name ?: "", data->type_name ? ":" : "", data->gres_cnt_node_alloc[0]);
		}
		list_iterator_destroy (i);
	} else if (g_job->job_gres_list) {
		i = list_iterator_create (g_job->job_gres_list);
		while ((gres = list_next (i))) {
			gres_job_state_t *data = gres->gres_data;
			dprintf(infofd, "%s %s%s%lu\n", gres->gres_name, data->type_name ?: "", data->type_name ? ":" : "", data->gres_cnt_node_alloc[0]);
		}
		list_iterator_destroy (i);
	}
	close(infofd);
}

extern int acct_gather_profile_p_node_step_start(stepd_step_rec_t* job)
{
	int rc = SLURM_SUCCESS;

	xassert(running_in_slurmstepd());
	if (!exporter_conf.dir)
		return rc;
	xassert(g_exporter_dirfd < 0);
	xassert(g_dirfd < 0);
	g_job = job;

	log_flag(PROFILE, "%s %s: option --profile=%s", plugin_type, __func__,
		 acct_gather_profile_to_string(g_job->profile));

	if (g_profile_running == ACCT_GATHER_PROFILE_NOT_SET)
		g_profile_running = _determine_profile();
	if (g_profile_running <= ACCT_GATHER_PROFILE_NONE)
		return rc;

	mkdir(exporter_conf.dir, 0755);
	if ((g_exporter_dirfd = open(exporter_conf.dir, O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC)) < 0) {
		warning("%s %s: could not open ProfileExporterDir '%s': %m", plugin_type, __func__, exporter_conf.dir);
		return SLURM_ERROR;
	}

	log_build_step_id_str(&g_job->step_id, g_step_id, sizeof(g_step_id), STEP_ID_FLAG_NO_PREFIX);
	mkdirat(g_exporter_dirfd, g_step_id, 0755);
	g_dirfd = openat(g_exporter_dirfd, g_step_id, O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
	if (g_dirfd < 0) {
		warning("%s %s: %s/%s: %m", plugin_type, __func__, exporter_conf.dir, g_step_id);
		return SLURM_ERROR;
	}

	write_job_info();
	/* do this again later once more fields are initialized */
	job_info_written = false;

	return rc;
}

extern int acct_gather_profile_p_child_forked(void)
{
	/* close any open fds (if not close on exec) */
	return SLURM_SUCCESS;
}

extern int acct_gather_profile_p_node_step_end(void)
{
	debug3("%s %s called with %d prof", plugin_type, __func__,
	       g_profile_running);

	xassert(running_in_slurmstepd());

	if (g_dirfd < 0)
		return SLURM_SUCCESS;
	int i;
	for (i = 0; i < tables_len; i++) {
		table_t *table = &tables[i];
		unlinkat(g_dirfd, table->name, 0);
	}
	unlinkat(g_dirfd, JOB_INFO_FILE_NAME, 0);
	close(g_dirfd);
	g_dirfd = -1;

	if (g_exporter_dirfd < 0)
		return SLURM_SUCCESS;
	unlinkat(g_exporter_dirfd, g_step_id, AT_REMOVEDIR);
	close(g_exporter_dirfd);
	g_exporter_dirfd = -1;

	return SLURM_SUCCESS;
}

extern int acct_gather_profile_p_task_start(uint32_t taskid)
{
	debug3("%s %s(%d) called with %d prof", plugin_type, __func__, taskid,
	       g_profile_running);

	return SLURM_SUCCESS;
}

extern int acct_gather_profile_p_task_end(pid_t taskpid)
{
	debug3("%s %s(%d) called", plugin_type, __func__, taskpid);

	if (g_dirfd < 0 || !g_job)
		return SLURM_SUCCESS;

	uint32_t i;
	char name[32];
	for (i = 0; i < g_job->node_tasks; i++) {
		if (g_job->task[i]->pid == taskpid) {
			snprintf(name, sizeof(name), "%u", i);
			unlinkat(g_dirfd, name, 0);
		}
	}

	return SLURM_SUCCESS;
}

extern int64_t acct_gather_profile_p_create_group(const char* name)
{
	if (!xstrcmp(name, "Tasks"))
		return GROUP_TASKS_ID;
	return 0;
}

extern int acct_gather_profile_p_create_dataset(const char* name,
						int64_t parent,
						acct_gather_profile_dataset_t
						*dataset)
{
	debug3("%s %s called", plugin_type, __func__);

	if (g_profile_running <= ACCT_GATHER_PROFILE_NONE)
		return SLURM_ERROR;

	int table_id = tables_len++;
	tables = xrealloc(tables, tables_len * sizeof(*tables));
	table_t *table = &tables[table_id];
	table->name = xstrdup(name);
	table->size = 0;
	if (parent == GROUP_TASKS_ID) {
		char *e;
		table->taskid = strtoul(name, &e, 10);
		if (e == name || *e)
			table->taskid = -1;
	} else {
		table->taskid = -1;
	}

	acct_gather_profile_dataset_t *dataset_loc = dataset;
	for (dataset_loc = dataset; dataset_loc && dataset_loc->type != PROFILE_FIELD_NOT_SET; dataset_loc++) {
		int idx = table->size++;
		table->dataset = xrealloc(table->dataset, table->size*sizeof(*table->dataset));
		table->dataset[idx].name = xstrdup(dataset_loc->name);
		table->dataset[idx].type = dataset_loc->type;
	}

	if (!job_info_written) {
		/* do this again now that more fields are initialized */
		write_job_info();
		job_info_written = true;
	}

	return table_id;
}

extern int acct_gather_profile_p_add_sample_data(int table_id, void *data,
						 time_t sample_time)
{
	debug3("%s %s called", plugin_type, __func__);

	xassert(running_in_slurmstepd());
	if (g_dirfd < 0 || !g_job || g_profile_running <= ACCT_GATHER_PROFILE_NONE)
		return SLURM_ERROR;
	xassert(table_id < tables_len);

	table_t *table = &tables[table_id];

	int infofd;
	infofd = openat(g_dirfd, table->name, O_WRONLY|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_TRUNC, 0644);
	if (infofd < 0) {
		warning("%s %s: write sample %s: %m", plugin_type, __func__, table->name);
		return SLURM_ERROR;
	}

	dprintf(infofd, "time %lu\n", sample_time);
	int i;
	for(i = 0; i < table->size; i++) {
		switch (table->dataset[i].type) {
		case PROFILE_FIELD_UINT64:
			dprintf(infofd, "%s %lu\n", table->dataset[i].name, ((union data_t *)data)[i].u);
			break;
		case PROFILE_FIELD_DOUBLE:
			dprintf(infofd, "%s %.2f\n", table->dataset[i].name, ((union data_t *)data)[i].d);
			break;
		case PROFILE_FIELD_NOT_SET:
			break;
		}
	}

	close(infofd);
	return SLURM_SUCCESS;
}

extern void acct_gather_profile_p_conf_values(List *data)
{
	config_key_pair_t *key_pair;

	xassert(*data);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("ProfileExporterDir");
	key_pair->value = xstrdup(exporter_conf.dir);
	list_append(*data, key_pair);

	key_pair = xmalloc(sizeof(config_key_pair_t));
	key_pair->name = xstrdup("ProfileExporterDefault");
	key_pair->value = xstrdup(acct_gather_profile_to_string(exporter_conf.def));
	list_append(*data, key_pair);

	return;
}

extern bool acct_gather_profile_p_is_active(uint32_t type)
{
	if (g_profile_running <= ACCT_GATHER_PROFILE_NONE)
		return false;
	return (type == ACCT_GATHER_PROFILE_NOT_SET) ||
		(g_profile_running & type);
}
