
#include "system.h"

#include <rpm/rpmmacro.h>
#include <rpm/rpmtypes.h>
#include <rpm/rpmlog.h>
#include <rpm/rpmstring.h>
#include <rpm/rpmts.h>

#include "lib/rpmplugins.h"

#define STR1(x) #x
#define STR(x) STR1(x)

struct rpmPlugin_s {
    char *name;
    void *handle;
};

struct rpmPlugins_s {
    rpmPlugin *plugins;
    int count;
    rpmts ts;
};

static rpmPlugin rpmpluginsGetPlugin(rpmPlugins plugins, const char *name)
{
    int i;
    for (i = 0; i < plugins->count; i++) {
	rpmPlugin plugin = plugins->plugins[i];
	if (rstreq(plugin->name, name)) {
	    return plugin;
	}
    }
    return NULL;
}

static int rpmpluginsHookIsSupported(void *handle, rpmPluginHook hook)
{
    rpmPluginHook *supportedHooks =
	(rpmPluginHook *) dlsym(handle, STR(PLUGIN_HOOKS));
    return (*supportedHooks & hook);
}

int rpmpluginsPluginAdded(rpmPlugins plugins, const char *name)
{
    return (rpmpluginsGetPlugin(plugins, name) != NULL);
}

rpmPlugins rpmpluginsNew(rpmts ts)
{
    rpmPlugins plugins = xcalloc(1, sizeof(*plugins));
    plugins->ts = ts;
    return plugins;
}

static rpmPlugin rpmPluginNew(const char *name, const char *path)
{
    rpmPlugin plugin;
    char *error;

    void *handle = dlopen(path, RTLD_LAZY);
    if (!handle) {
	rpmlog(RPMLOG_ERR, _("Failed to dlopen %s %s\n"), path, dlerror());
	return NULL;
    }

    /* make sure the plugin has the supported hooks flag */
    (void) dlsym(handle, STR(PLUGIN_HOOKS));
    if ((error = dlerror()) != NULL) {
	rpmlog(RPMLOG_ERR, _("Failed to resolve symbol %s: %s\n"),
	       STR(PLUGIN_HOOKS), error);
	return NULL;
    }

    plugin = xcalloc(1, sizeof(*plugin));
    plugin->name = xstrdup(name);
    plugin->handle = handle;

    return plugin;
}

static rpmPlugin rpmPluginFree(rpmPlugin plugin)
{
    if (plugin) {
	dlclose(plugin->handle);
	free(plugin->name);
	free(plugin);
    }
    return NULL;
}

rpmRC rpmpluginsAdd(rpmPlugins plugins, const char *name, const char *path,
		    const char *opts)
{
    rpmPlugin plugin = rpmPluginNew(name, path);;

    if (plugin == NULL)
	return RPMRC_FAIL;
    
    plugins->plugins = xrealloc(plugins->plugins,
			    (plugins->count + 1) * sizeof(*plugins->plugins));
    plugins->plugins[plugins->count] = plugin;
    plugins->count++;

    return rpmpluginsCallInit(plugins, name, opts);
}

rpmRC rpmpluginsAddPlugin(rpmPlugins plugins, const char *type, const char *name)
{
    char *path;
    char *options;
    rpmRC rc = RPMRC_FAIL;

    path = rpmExpand("%{?__", type, "_", name, "}", NULL);
    if (!path || rstreq(path, "")) {
	rpmlog(RPMLOG_ERR, _("Failed to expand %%__%s_%s macro\n"),
	       type, name);
	goto exit;
    }

    /* split the options from the path */
#define SKIPSPACE(s)    { while (*(s) &&  risspace(*(s))) (s)++; }
#define SKIPNONSPACE(s) { while (*(s) && !risspace(*(s))) (s)++; }
    options = path;
    SKIPNONSPACE(options);
    if (risspace(*options)) {
	*options = '\0';
	options++;
	SKIPSPACE(options);
    }
    if (*options == '\0') {
	options = NULL;
    }

    rc = rpmpluginsAdd(plugins, name, path, options);

  exit:
    _free(path);
    return rc;
}

rpmPlugins rpmpluginsFree(rpmPlugins plugins)
{
    int i;
    for (i = 0; i < plugins->count; i++) {
	rpmPlugin plugin = plugins->plugins[i];
	rpmpluginsCallCleanup(plugins, plugin->name);
	rpmPluginFree(plugin);
    }
    plugins->plugins = _free(plugins->plugins);
    plugins->ts = NULL;
    _free(plugins);

    return NULL;
}

#define RPMPLUGINS_GET_PLUGIN(name) \
	plugin = rpmpluginsGetPlugin(plugins, name); \
	if (plugin == NULL || plugin->handle == NULL) { \
		rpmlog(RPMLOG_ERR, _("Plugin %s not loaded\n"), name); \
		return RPMRC_FAIL; \
	}

/* Common define for all rpmpluginsCall* hook functions */
#define RPMPLUGINS_SET_HOOK_FUNC(hook) \
	char * error; \
	if (!rpmpluginsHookIsSupported(plugin->handle, hook)) { \
		return RPMRC_OK; \
	} \
	*(void **)(&hookFunc) = dlsym(plugin->handle, STR(hook##_FUNC)); \
	if ((error = dlerror()) != NULL) { \
		rpmlog(RPMLOG_ERR, _("Failed to resolve %s plugin symbol %s: %s\n"), plugin->name, STR(hook##_FUNC), error); \
		return RPMRC_FAIL; \
	} \
	if (rpmtsFlags(plugins->ts) & (RPMTRANS_FLAG_TEST | RPMTRANS_FLAG_JUSTDB)) { \
		return RPMRC_OK; \
	} \
	rpmlog(RPMLOG_DEBUG, "Plugin: calling hook %s in %s plugin\n", STR(hook##_FUNC), plugin->name);

rpmRC rpmpluginsCallInit(rpmPlugins plugins, const char *name, const char *opts)
{
    plugin_init_func hookFunc;
    rpmPlugin plugin = NULL;
    RPMPLUGINS_GET_PLUGIN(name);
    RPMPLUGINS_SET_HOOK_FUNC(PLUGINHOOK_INIT);
    return hookFunc(plugins->ts, name, opts);
}

rpmRC rpmpluginsCallCleanup(rpmPlugins plugins, const char *name)
{
    plugin_cleanup_func hookFunc;;
    rpmPlugin plugin = NULL;
    RPMPLUGINS_GET_PLUGIN(name);
    RPMPLUGINS_SET_HOOK_FUNC(PLUGINHOOK_CLEANUP);
    return hookFunc();
}

rpmRC rpmpluginsCallOpenTE(rpmPlugins plugins, const char *name, rpmte te)
{
    plugin_opente_func hookFunc;
    rpmPlugin plugin = NULL;
    RPMPLUGINS_GET_PLUGIN(name);
    RPMPLUGINS_SET_HOOK_FUNC(PLUGINHOOK_OPENTE);
    return hookFunc(te);
}

rpmRC rpmpluginsCallCollectionPostAdd(rpmPlugins plugins, const char *name)
{
    plugin_coll_post_add_func hookFunc;
    rpmPlugin plugin = NULL;
    RPMPLUGINS_GET_PLUGIN(name);
    RPMPLUGINS_SET_HOOK_FUNC(PLUGINHOOK_COLL_POST_ADD);
    return hookFunc();
}

rpmRC rpmpluginsCallCollectionPostAny(rpmPlugins plugins, const char *name)
{
    plugin_coll_post_any_func hookFunc;
    rpmPlugin plugin = NULL;
    RPMPLUGINS_GET_PLUGIN(name);
    RPMPLUGINS_SET_HOOK_FUNC(PLUGINHOOK_COLL_POST_ANY);
    return hookFunc();
}

rpmRC rpmpluginsCallCollectionPreRemove(rpmPlugins plugins, const char *name)
{
    plugin_coll_pre_remove_func hookFunc;
    rpmPlugin plugin = NULL;
    RPMPLUGINS_GET_PLUGIN(name);
    RPMPLUGINS_SET_HOOK_FUNC(PLUGINHOOK_COLL_PRE_REMOVE);
    return hookFunc();
}

rpmRC rpmpluginsCallTsmPre(rpmPlugins plugins, rpmts ts)
{
    plugin_tsm_pre_func hookFunc;
    int i;
    rpmRC rc = RPMRC_OK;

    for (i = 0; i < plugins->count; i++) {
	rpmPlugin plugin = plugins->plugins[i];
	RPMPLUGINS_SET_HOOK_FUNC(PLUGINHOOK_TSM_PRE);
	if (hookFunc(ts) == RPMRC_FAIL)
	    rc = RPMRC_FAIL;
    }

    return rc;
}

rpmRC rpmpluginsCallTsmPost(rpmPlugins plugins, rpmts ts, int res)
{
    plugin_tsm_post_func hookFunc;
    int i;
    rpmRC rc = RPMRC_OK;

    for (i = 0; i < plugins->count; i++) {
	rpmPlugin plugin = plugins->plugins[i];
	RPMPLUGINS_SET_HOOK_FUNC(PLUGINHOOK_TSM_POST);
	if (hookFunc(ts, res) == RPMRC_FAIL)
	    rc = RPMRC_FAIL;
    }

    return rc;
}

rpmRC rpmpluginsCallPsmPre(rpmPlugins plugins, rpmte te)
{
    plugin_psm_pre_func hookFunc;
    int i;
    rpmRC rc = RPMRC_OK;

    for (i = 0; i < plugins->count; i++) {
	rpmPlugin plugin = plugins->plugins[i];
	RPMPLUGINS_SET_HOOK_FUNC(PLUGINHOOK_PSM_PRE);
	if (hookFunc(te) == RPMRC_FAIL)
	    rc = RPMRC_FAIL;
    }

    return rc;
}

rpmRC rpmpluginsCallPsmPost(rpmPlugins plugins, rpmte te, int res)
{
    plugin_psm_post_func hookFunc;
    int i;
    rpmRC rc = RPMRC_OK;

    for (i = 0; i < plugins->count; i++) {
	rpmPlugin plugin = plugins->plugins[i];
	RPMPLUGINS_SET_HOOK_FUNC(PLUGINHOOK_PSM_POST);
	if (hookFunc(te, res) == RPMRC_FAIL)
	    rc = RPMRC_FAIL;
    }

    return rc;
}

rpmRC rpmpluginsCallScriptletPre(rpmPlugins plugins, const char *s_name, int type)
{
    plugin_scriptlet_pre_func hookFunc;
    int i;
    rpmRC rc = RPMRC_OK;

    for (i = 0; i < plugins->count; i++) {
	rpmPlugin plugin = plugins->plugins[i];
	RPMPLUGINS_SET_HOOK_FUNC(PLUGINHOOK_SCRIPTLET_PRE);
	if (hookFunc(s_name, type) == RPMRC_FAIL)
	    rc = RPMRC_FAIL;
    }

    return rc;
}

rpmRC rpmpluginsCallScriptletForkPost(rpmPlugins plugins, const char *path, int type)
{
    plugin_scriptlet_fork_post_func hookFunc;
    int i;
    rpmRC rc = RPMRC_OK;

    for (i = 0; i < plugins->count; i++) {
	rpmPlugin plugin = plugins->plugins[i];
	RPMPLUGINS_SET_HOOK_FUNC(PLUGINHOOK_SCRIPTLET_FORK_POST);
	if (hookFunc(path, type) == RPMRC_FAIL)
	    rc = RPMRC_FAIL;
    }

    return rc;
}

rpmRC rpmpluginsCallScriptletPost(rpmPlugins plugins, const char *s_name, int type, int res)
{
    plugin_scriptlet_post_func hookFunc;
    int i;
    rpmRC rc = RPMRC_OK;

    for (i = 0; i < plugins->count; i++) {
	rpmPlugin plugin = plugins->plugins[i];
	RPMPLUGINS_SET_HOOK_FUNC(PLUGINHOOK_SCRIPTLET_POST);
	if (hookFunc(s_name, type, res) == RPMRC_FAIL)
	    rc = RPMRC_FAIL;
    }

    return rc;
}

rpmRC rpmpluginsCallFsmFilePre(rpmPlugins plugins, const char* path,
                                mode_t file_mode, rpmFsmOp op)
{
    plugin_fsm_file_pre_func hookFunc;
    int i;
    rpmRC rc = RPMRC_OK;

    for (i = 0; i < plugins->count; i++) {
	rpmPlugin plugin = plugins->plugins[i];
	RPMPLUGINS_SET_HOOK_FUNC(PLUGINHOOK_FSM_FILE_PRE);
	if (hookFunc(path, file_mode, op) == RPMRC_FAIL)
	    rc = RPMRC_FAIL;
    }

    return rc;
}

rpmRC rpmpluginsCallFsmFilePost(rpmPlugins plugins, const char* path,
                                mode_t file_mode, rpmFsmOp op, int res)
{
    plugin_fsm_file_post_func hookFunc;
    int i;
    rpmRC rc = RPMRC_OK;

    for (i = 0; i < plugins->count; i++) {
	rpmPlugin plugin = plugins->plugins[i];
	RPMPLUGINS_SET_HOOK_FUNC(PLUGINHOOK_FSM_FILE_POST);
	if (hookFunc(path, file_mode, op, res) == RPMRC_FAIL)
	    rc = RPMRC_FAIL;
    }

    return rc;
}

rpmRC rpmpluginsCallFsmFilePrepare(rpmPlugins plugins,
				   const char *path, const char *dest,
				   mode_t file_mode, rpmFsmOp op)
{
    plugin_fsm_file_prepare_func hookFunc;
    int i;
    rpmRC rc = RPMRC_OK;

    for (i = 0; i < plugins->count; i++) {
	rpmPlugin plugin = plugins->plugins[i];
	RPMPLUGINS_SET_HOOK_FUNC(PLUGINHOOK_FSM_FILE_PREPARE);
	if (hookFunc(path, dest, file_mode, op) == RPMRC_FAIL)
	    rc = RPMRC_FAIL;
    }

    return rc;
}
