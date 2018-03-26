#include "ManifestGeneration.h"
#include "../Configuration.h"
#include "cxx_supportlib/Constants.h"
#include "cxx_supportlib/FileTools/PathManipCBindings.h"


static PsgJsonValue *
generate_config_manifest(ngx_conf_t *cf, passenger_loc_conf_t *toplevel_plcf) {
    manifest_gen_ctx_t ctx;
    PsgJsonValue *result;

    init_config_manifest_generation(cf, &ctx);

    generate_config_manifest_for_autogenerated_main_conf(&ctx, &passenger_main_conf);
    recursively_generate_config_manifest_for_loc_conf(&ctx, toplevel_plcf);

    reverse_manifest_value_hierarchies(&ctx);
    set_manifest_autogenerated_global_conf_defaults(&ctx);
    set_manifest_autogenerated_app_conf_defaults(&ctx,
        ctx.default_app_config_container);
    set_manifest_autogenerated_loc_conf_defaults(&ctx,
        ctx.default_loc_config_container);
    manifest_inherit_application_value_hierarchies(&ctx);
    manifest_inherit_location_value_hierarchies(&ctx);

    result = ctx.manifest;
    deinit_config_manifest_generation(&ctx);
    return result;
}


static void
init_config_manifest_generation(ngx_conf_t *cf, manifest_gen_ctx_t *ctx) {
    ngx_memzero(ctx, sizeof(manifest_gen_ctx_t));

    ctx->cf = cf;
    ctx->manifest = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_OBJECT);

    ctx->empty_object = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_OBJECT);
    ctx->empty_array  = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_ARRAY);

    ctx->it   = psg_json_value_iterator_new();
    ctx->end  = psg_json_value_iterator_new();
    ctx->it2  = psg_json_value_iterator_new();
    ctx->end2 = psg_json_value_iterator_new();
    ctx->it3  = psg_json_value_iterator_new();
    ctx->end3 = psg_json_value_iterator_new();
    ctx->it4  = psg_json_value_iterator_new();
    ctx->end4 = psg_json_value_iterator_new();

    ctx->global_config_container = psg_json_value_set_value(ctx->manifest,
        "global_configuration", -1, ctx->empty_object);
    ctx->default_app_config_container = psg_json_value_set_value(ctx->manifest,
        "default_application_configuration", -1, ctx->empty_object);
    ctx->default_loc_config_container = psg_json_value_set_value(ctx->manifest,
        "default_location_configuration", -1, ctx->empty_object);
    ctx->app_configs_container = psg_json_value_set_value(ctx->manifest,
        "application_configurations", -1, ctx->empty_object);
}

static void
deinit_config_manifest_generation(manifest_gen_ctx_t *ctx) {
    psg_json_value_free(ctx->empty_object);
    psg_json_value_free(ctx->empty_array);
    psg_json_value_iterator_free(ctx->it);
    psg_json_value_iterator_free(ctx->end);
    psg_json_value_iterator_free(ctx->it2);
    psg_json_value_iterator_free(ctx->end2);
    psg_json_value_iterator_free(ctx->it3);
    psg_json_value_iterator_free(ctx->end3);
    psg_json_value_iterator_free(ctx->it4);
    psg_json_value_iterator_free(ctx->end4);
}


static void
recursively_generate_config_manifest_for_loc_conf(manifest_gen_ctx_t *ctx,
    passenger_loc_conf_t *plcf)
{
    passenger_loc_conf_t **children;
    ngx_http_core_srv_conf_t *cscf;
    ngx_http_core_loc_conf_t *clcf;
    ngx_uint_t i;

    cscf = plcf->cscf;
    clcf = plcf->clcf;

    if (cscf != NULL && clcf != NULL && plcf->autogenerated.enabled) {
        generate_config_manifest_for_loc_conf(ctx, plcf, cscf, clcf);
    }

    children = plcf->children.elts;
    for (i = 0; i < plcf->children.nelts; i++) {
        recursively_generate_config_manifest_for_loc_conf(ctx, children[i]);
    }
}

static int
infer_loc_conf_app_group_name(manifest_gen_ctx_t *ctx, passenger_loc_conf_t *plcf,
    ngx_http_core_loc_conf_t *clcf, ngx_str_t *result)
{
    ngx_str_t app_root, app_env;
    char *abs_path;
    u_char *buf;
    void *_unused;
    size_t buf_size;

    if (plcf->autogenerated.app_group_name.data == NULL) {
        if (plcf->autogenerated.app_root.data == NULL) {
            buf_size = clcf->root.len + sizeof("/..") - 1;

            buf = (u_char *) ngx_pnalloc(ctx->cf->pool, buf_size);
            if (buf == NULL) {
                return 0;
            }
            app_root.data = buf;
            app_root.len = ngx_snprintf(buf, buf_size, "%V/..", &clcf->root) - buf;
        } else {
            app_root = plcf->autogenerated.app_root;
        }

        abs_path = psg_absolutize_path(
            (const char *) app_root.data, app_root.len,
            (const char *) ctx->cf->cycle->prefix.data, ctx->cf->cycle->prefix.len,
            &app_root.len);
        app_root.data = (u_char *) ngx_pnalloc(ctx->cf->pool, app_root.len);
        _unused = ngx_copy(app_root.data, abs_path, app_root.len);
        (void) _unused; /* Shut up compiler warning */
        free(abs_path);

        if (plcf->autogenerated.environment.data == NULL) {
            app_env.data = (u_char *) DEFAULT_APP_ENV;
            app_env.len = sizeof(DEFAULT_APP_ENV) - 1;
        } else {
            app_env = plcf->autogenerated.environment;
        }

        buf_size = app_root.len + app_env.len + sizeof(" ()") - 1;
        buf = (u_char *) ngx_pnalloc(ctx->cf->pool, buf_size);
        result->data = buf;
        result->len = ngx_snprintf(buf, buf_size, "%V (%V)", &app_root, &app_env) - buf;
    } else {
        *result = plcf->autogenerated.app_group_name;
    }

    return 1;
}

static u_char *
infer_default_app_root(manifest_gen_ctx_t *ctx, ngx_http_core_loc_conf_t *clcf,
    size_t *len)
{
    u_char *path, *end;

    path = ngx_pnalloc(ctx->cf->temp_pool, clcf->root.len + 3);
    end = ngx_snprintf(path, clcf->root.len + 3,
        "%V/..", &clcf->root);
    return (u_char *) psg_absolutize_path((const char *) path,
        end - path, NULL, 0, len);
}

static PsgJsonValue *
find_or_create_manifest_app_config_container(manifest_gen_ctx_t *ctx,
    ngx_str_t *app_group_name)
{
    PsgJsonValue *result;

    result = psg_json_value_get_or_create_null(ctx->app_configs_container,
        (const char *) app_group_name->data, app_group_name->len);
    if (psg_json_value_is_null(result)) {
        psg_json_value_set_value(result, "options", -1, ctx->empty_object);
        psg_json_value_set_value(result, "default_location_configuration", -1, ctx->empty_object);
        psg_json_value_set_value(result, "location_configurations", -1, ctx->empty_array);
    }
    return result;
}

static PsgJsonValue *
find_or_create_manifest_loc_config_container(manifest_gen_ctx_t *ctx,
    PsgJsonValue *app_config_container, ngx_http_core_srv_conf_t *cscf,
    ngx_http_core_loc_conf_t *clcf)
{
    PsgJsonValue *loc_configs_container;
    PsgJsonValue *loc_config_container;

    loc_configs_container = psg_json_value_get(app_config_container,
        "location_configurations", -1);
    loc_config_container = find_manifest_loc_config_container(ctx,
        loc_configs_container, cscf, clcf);
    if (loc_config_container == NULL) {
        loc_config_container = create_manifest_loc_config_container(ctx,
            loc_configs_container, cscf, clcf);
    }

    return loc_config_container;
}

static int
matches_any_server_names(manifest_gen_ctx_t *ctx, ngx_http_core_srv_conf_t *cscf,
    PsgJsonValue *server_names_doc)
{
    ngx_http_server_name_t *server_names = cscf->server_names.elts;
    PsgJsonValue *server_name_doc;
    ngx_str_t server_name;
    ngx_uint_t i;

    psg_json_value_begin(server_names_doc, ctx->it2);
    psg_json_value_end(server_names_doc, ctx->end2);

    while (!psg_json_value_iterator_eq(ctx->it2, ctx->end2)) {
        server_name_doc = psg_json_value_iterator_get_value(ctx->it2);
        server_name.data = (u_char *) psg_json_value_get_str(server_name_doc, &server_name.len);

        for (i = 0; i < cscf->server_names.nelts; i++) {
            if (server_names[i].name.len == server_name.len
                && ngx_strncasecmp(server_names[i].name.data, server_name.data,
                                   server_name.len) == 0)
            {
                return 1;
            }
        }

        psg_json_value_iterator_advance(ctx->it2);
    }

    return 0;
}

static PsgJsonValue *
find_manifest_loc_config_container(manifest_gen_ctx_t *ctx,
    PsgJsonValue *loc_configs_container, ngx_http_core_srv_conf_t *cscf,
    ngx_http_core_loc_conf_t *clcf)
{
    PsgJsonValue *loc_config_container;
    PsgJsonValue *vhost_doc;
    PsgJsonValue *server_names_doc;
    PsgJsonValue *location_matcher_doc;
    ngx_str_t json_location_matcher_type, json_location_matcher_value;

    psg_json_value_begin(loc_configs_container, ctx->it);
    psg_json_value_end(loc_configs_container, ctx->end);

    while (!psg_json_value_iterator_eq(ctx->it, ctx->end)) {
        loc_config_container = psg_json_value_iterator_get_value(ctx->it);
        vhost_doc = psg_json_value_get(loc_config_container, "web_server_virtual_host", -1);
        location_matcher_doc = psg_json_value_get(loc_config_container, "location_matcher", -1);

        json_location_matcher_type.data = (u_char *) psg_json_value_get_str(
            psg_json_value_get(location_matcher_doc, "type", -1),
            &json_location_matcher_type.len);
        #if (NGX_PCRE)
            if (clcf->regex != NULL) {
                if (json_location_matcher_type.len != sizeof("regex") - 1
                    || ngx_memcmp(json_location_matcher_type.data, "regex", sizeof("regex") - 1) != 0)
                {
                    goto no_match;
                }
            } else
        #endif
        if (clcf->exact_match) {
            if (json_location_matcher_type.len != sizeof("exact") - 1
                || ngx_memcmp(json_location_matcher_type.data, "exact", sizeof("exact") - 1) != 0)
            {
                goto no_match;
            }
        } else {
            if (json_location_matcher_type.len != sizeof("prefix") - 1
                || ngx_memcmp(json_location_matcher_type.data, "prefix", sizeof("prefix") - 1) != 0)
            {
                goto no_match;
            }
        }

        json_location_matcher_value.data = (u_char *) psg_json_value_get_str(
            psg_json_value_get(location_matcher_doc, "value", -1),
            &json_location_matcher_value.len);
        if (ngx_memn2cmp(clcf->name.data, json_location_matcher_value.data,
                         clcf->name.len, json_location_matcher_value.len)
            != 0)
        {
            goto no_match;
        }

        server_names_doc = psg_json_value_get(vhost_doc, "server_names", -1);
        if (!matches_any_server_names(ctx, cscf, server_names_doc)) {
            goto no_match;
        }

        return loc_config_container;

        no_match:
        psg_json_value_iterator_advance(ctx->it);
    }

    return NULL;
}

static PsgJsonValue *
create_manifest_loc_config_container(manifest_gen_ctx_t *ctx,
    PsgJsonValue *loc_configs_container, ngx_http_core_srv_conf_t *cscf,
    ngx_http_core_loc_conf_t *clcf)
{
    PsgJsonValue *loc_config_container = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_OBJECT);
    PsgJsonValue *vhost_doc = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_OBJECT);
    PsgJsonValue *server_names_doc = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_ARRAY);
    PsgJsonValue *location_matcher_doc = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_OBJECT);
    PsgJsonValue *options_doc = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_OBJECT);
    PsgJsonValue *server_name_doc, *result;
    ngx_http_server_name_t *server_names = cscf->server_names.elts;
    ngx_uint_t i;

    for (i = 0; i < cscf->server_names.nelts; i++) {
        server_name_doc = psg_json_value_new_str((const char *) server_names[i].name.data,
            server_names[i].name.len);
        psg_json_value_append_val(server_names_doc, server_name_doc);
        psg_json_value_free(server_name_doc);
    }
    psg_json_value_set_value(vhost_doc, "server_names", -1, server_names_doc);

    psg_json_value_set_str(location_matcher_doc, "value",
        (const char *) clcf->name.data, clcf->name.len);
    #if (NGX_PCRE)
        if (clcf->regex != NULL) {
            psg_json_value_set_str(location_matcher_doc, "type",
                "regex", -1);
        } else
    #endif
    if (clcf->exact_match) {
        psg_json_value_set_str(location_matcher_doc, "type",
            "exact", -1);
    } else {
        psg_json_value_set_str(location_matcher_doc, "type",
            "prefix", -1);
    }

    psg_json_value_set_value(loc_config_container, "web_server_virtual_host", -1, vhost_doc);
    psg_json_value_set_value(loc_config_container, "location_matcher", -1, location_matcher_doc);
    psg_json_value_set_value(loc_config_container, "options", -1, options_doc);
    result = psg_json_value_append_val(loc_configs_container, loc_config_container);
    psg_json_value_free(loc_config_container);
    psg_json_value_free(vhost_doc);
    psg_json_value_free(server_names_doc);
    psg_json_value_free(location_matcher_doc);
    psg_json_value_free(options_doc);
    return result;
}

static PsgJsonValue *
find_or_create_manifest_option_container(manifest_gen_ctx_t *ctx,
    PsgJsonValue *options_container, const char *option_name,
    size_t option_name_len)
{
    PsgJsonValue *result;

    result = psg_json_value_get_or_create_null(options_container,
        option_name, option_name_len);
    if (psg_json_value_is_null(result)) {
        init_manifest_option_container(ctx, result);
    }

    return result;
}

static void
init_manifest_option_container(manifest_gen_ctx_t *ctx, PsgJsonValue *doc) {
    psg_json_value_set_value(doc, "value_hierarchy", -1, ctx->empty_array);
}

static PsgJsonValue *
add_manifest_option_container_hierarchy_member(PsgJsonValue *option_container,
    ngx_str_t *source_file, ngx_uint_t source_line)
{
    PsgJsonValue *value_hierarchy = psg_json_value_get(option_container, "value_hierarchy", -1);
    PsgJsonValue *hierarchy_member = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_OBJECT);
    PsgJsonValue *source = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_OBJECT);
    PsgJsonValue *result;

    psg_json_value_set_str(source, "type", "web-server-config", sizeof("web-server-config") - 1);
    psg_json_value_set_str(source, "path", (const char *) source_file->data, source_file->len);
    psg_json_value_set_uint(source, "line", source_line);
    psg_json_value_set_value(hierarchy_member, "source", -1, source);
    result = psg_json_value_append_val(value_hierarchy, hierarchy_member);

    psg_json_value_free(hierarchy_member);
    psg_json_value_free(source);

    return result;
}

static void
generate_config_manifest_for_loc_conf(manifest_gen_ctx_t *ctx,
    passenger_loc_conf_t *plcf, ngx_http_core_srv_conf_t *cscf,
    ngx_http_core_loc_conf_t *clcf)
{
    generate_config_manifest_for_autogenerated_loc_conf(ctx, plcf, cscf, clcf);
}

static void
find_or_create_manifest_app_and_loc_options_containers(manifest_gen_ctx_t *ctx,
    passenger_loc_conf_t *plcf, ngx_http_core_srv_conf_t *cscf,
    ngx_http_core_loc_conf_t *clcf, PsgJsonValue **app_options_container,
    PsgJsonValue **loc_options_container)
{
    ngx_str_t app_group_name;
    PsgJsonValue *app_config_container, *loc_config_container;
    ngx_str_t default_app_root;

    if (*app_options_container != NULL && *loc_options_container != NULL) {
        return;
    }

    if (cscf->server_name.len == 0) {
        /* We are in the global context */
        *app_options_container = ctx->default_app_config_container;
        *loc_options_container = ctx->default_loc_config_container;
    } else if (clcf->name.len == 0) {
        /* We are in a server block */
        infer_loc_conf_app_group_name(ctx, plcf, clcf, &app_group_name);
        app_config_container = find_or_create_manifest_app_config_container(ctx, &app_group_name);
        *app_options_container = psg_json_value_get(app_config_container, "options", -1);
        *loc_options_container = psg_json_value_get(app_config_container, "default_location_configuration", -1);

        /* Create a default value for passenger_app_root
         * if we just created this config container
         */
        if (psg_json_value_size(*app_options_container) == 0) {
            add_manifest_options_container_static_default_str(ctx,
                *app_options_container,
                "passenger_app_group_name", -1,
                (const char *) app_group_name.data, app_group_name.len);

            default_app_root.data = infer_default_app_root(
                ctx, clcf, &default_app_root.len);
            add_manifest_options_container_static_default_str(ctx,
                *app_options_container,
                "passenger_app_root", -1,
                (const char *) default_app_root.data, default_app_root.len);
            free(default_app_root.data);
        }
    } else {
        /* We are in a location/if block */
        infer_loc_conf_app_group_name(ctx, plcf, clcf, &app_group_name);
        app_config_container = find_or_create_manifest_app_config_container(ctx, &app_group_name);
        loc_config_container = find_or_create_manifest_loc_config_container(ctx, app_config_container,
            cscf, clcf);
        *app_options_container = psg_json_value_get(app_config_container, "options", -1);
        *loc_options_container = psg_json_value_get(loc_config_container, "options", -1);
    }
}


static void
reverse_manifest_value_hierarchies(manifest_gen_ctx_t *ctx) {
    PsgJsonValue *app_config_container;
    PsgJsonValue *options_container;
    PsgJsonValue *location_configs_container, *location_config_container;

    reverse_value_hierarchies_in_options_container(ctx->global_config_container,
        ctx->it, ctx->end);
    reverse_value_hierarchies_in_options_container(ctx->default_app_config_container,
        ctx->it, ctx->end);
    reverse_value_hierarchies_in_options_container(ctx->default_loc_config_container,
        ctx->it, ctx->end);

    psg_json_value_begin(ctx->app_configs_container, ctx->it);
    psg_json_value_end(ctx->app_configs_container, ctx->end);
    while (!psg_json_value_iterator_eq(ctx->it, ctx->end)) {
        app_config_container = psg_json_value_iterator_get_value(ctx->it);

        options_container = psg_json_value_get(app_config_container,
            "options", -1);
        reverse_value_hierarchies_in_options_container(options_container,
            ctx->it2, ctx->end2);

        options_container = psg_json_value_get(app_config_container,
            "default_location_configuration", -1);
        reverse_value_hierarchies_in_options_container(options_container,
            ctx->it2, ctx->end2);

        location_configs_container = psg_json_value_get(app_config_container,
            "location_configurations", -1);
        if (location_configs_container != NULL) {
            psg_json_value_begin(location_configs_container, ctx->it2);
            psg_json_value_end(location_configs_container, ctx->end2);
            while (!psg_json_value_iterator_eq(ctx->it2, ctx->end2)) {
                location_config_container = psg_json_value_iterator_get_value(ctx->it2);

                options_container = psg_json_value_get(location_config_container,
                    "options", -1);
                reverse_value_hierarchies_in_options_container(options_container,
                    ctx->it3, ctx->end3);

                psg_json_value_iterator_advance(ctx->it2);
            }
        }

        psg_json_value_iterator_advance(ctx->it);
    }
}

static void
reverse_value_hierarchies_in_options_container(PsgJsonValue *options_container,
    PsgJsonValueIterator *it, PsgJsonValueIterator *end)
{
    PsgJsonValue *option_container, *value_hierarchy_doc;
    unsigned int i, len;

    psg_json_value_begin(options_container, it);
    psg_json_value_end(options_container, end);
    while (!psg_json_value_iterator_eq(it, end)) {
        option_container = psg_json_value_iterator_get_value(it);
        value_hierarchy_doc = psg_json_value_get(option_container,
            "value_hierarchy", -1);
        len = psg_json_value_size(value_hierarchy_doc);

        for (i = 0; i < len / 2; i++) {
            psg_json_value_swap(
                psg_json_value_get_at_index(value_hierarchy_doc, i),
                psg_json_value_get_at_index(value_hierarchy_doc, len - i - 1));
        }

        psg_json_value_iterator_advance(it);
    }
}

static void
add_manifest_options_container_dynamic_default(manifest_gen_ctx_t *ctx,
    PsgJsonValue *options_container,
    const char *option_name, size_t option_name_len,
    const char *desc, size_t desc_len)
{
    PsgJsonValue *option_container, *hierarchy, *hierarchy_member, *source;

    option_container = psg_json_value_get_or_create_null(options_container, option_name, option_name_len);
    if (psg_json_value_is_null(option_container)) {
        init_manifest_option_container(ctx, option_container);
    }
    hierarchy = psg_json_value_get(option_container, "value_hierarchy", -1);

    source = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_OBJECT);
    psg_json_value_set_str(source, "type", "dynamic-default-description", -1);

    hierarchy_member = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_OBJECT);
    psg_json_value_set_value(hierarchy_member, "source", -1, source);
    psg_json_value_set_str(hierarchy_member, "value", desc, desc_len);

    psg_json_value_append_val(hierarchy, hierarchy_member);

    psg_json_value_free(hierarchy_member);
    psg_json_value_free(source);
}

static PsgJsonValue *
add_manifest_options_container_static_default(manifest_gen_ctx_t *ctx,
    PsgJsonValue *options_container, const char *option_name, size_t option_name_len)
{
    PsgJsonValue *option_container, *hierarchy, *hierarchy_member, *source, *result;

    option_container = psg_json_value_get_or_create_null(options_container, option_name, option_name_len);
    if (psg_json_value_is_null(option_container)) {
        init_manifest_option_container(ctx, option_container);
    }
    hierarchy = psg_json_value_get(option_container, "value_hierarchy", -1);

    source = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_OBJECT);
    psg_json_value_set_str(source, "type", "default", -1);

    hierarchy_member = psg_json_value_new_with_type(PSG_JSON_VALUE_TYPE_OBJECT);
    psg_json_value_set_value(hierarchy_member, "source", -1, source);

    result = psg_json_value_append_val(hierarchy, hierarchy_member);

    psg_json_value_free(hierarchy_member);
    psg_json_value_free(source);

    return result;
}

static void
add_manifest_options_container_static_default_str(manifest_gen_ctx_t *ctx,
    PsgJsonValue *options_container, const char *option_name, size_t option_name_len,
    const char *value, size_t value_len)
{
    PsgJsonValue *hierarchy_member = add_manifest_options_container_static_default(
        ctx, options_container, option_name, option_name_len);
    psg_json_value_set_str(hierarchy_member, "value", value, value_len);
}

static void
add_manifest_options_container_static_default_int(manifest_gen_ctx_t *ctx,
    PsgJsonValue *options_container, const char *option_name, size_t option_name_len,
    int value)
{
    PsgJsonValue *hierarchy_member = add_manifest_options_container_static_default(
        ctx, options_container, option_name, option_name_len);
    psg_json_value_set_int(hierarchy_member, "value", value);
}

static void
add_manifest_options_container_static_default_uint(manifest_gen_ctx_t *ctx,
    PsgJsonValue *options_container, const char *option_name, size_t option_name_len,
    unsigned int value)
{
    PsgJsonValue *hierarchy_member = add_manifest_options_container_static_default(
        ctx, options_container, option_name, option_name_len);
    psg_json_value_set_uint(hierarchy_member, "value", value);
}

static void
add_manifest_options_container_static_default_bool(manifest_gen_ctx_t *ctx,
    PsgJsonValue *options_container, const char *option_name, size_t option_name_len,
    int value)
{
    PsgJsonValue *hierarchy_member = add_manifest_options_container_static_default(
        ctx, options_container, option_name, option_name_len);
    psg_json_value_set_bool(hierarchy_member, "value", value);
}

static void
manifest_inherit_application_value_hierarchies(manifest_gen_ctx_t *ctx) {
    PsgJsonValue *app_config_container, *options_container, *option_container, *default_app_config;
    PsgJsonValue *value_hierarchy_doc, *value_hierarchy_from_default;
    const char *option_name;
    size_t option_name_len;

    /* Iterate through all 'application_configurations' objects */
    psg_json_value_begin(ctx->app_configs_container, ctx->it);
    psg_json_value_end(ctx->app_configs_container, ctx->end);
    while (!psg_json_value_iterator_eq(ctx->it, ctx->end)) {
        app_config_container = psg_json_value_iterator_get_value(ctx->it);

        /* Iterate through all its 'options' objects */
        options_container = psg_json_value_get(app_config_container, "options", -1);
        psg_json_value_begin(options_container, ctx->it2);
        psg_json_value_end(options_container, ctx->end2);
        while (!psg_json_value_iterator_eq(ctx->it2, ctx->end2)) {
            /* For each option, inherit the value hierarchies
             * from the 'default_application_configuration' object.
             *
             * Since the value hierarchy array is already in
             * most-to-least-specific order, simply appending
             * the 'default_application_configuration' hierarchy is
             * enough.
             */
            option_name = psg_json_value_iterator_get_name(ctx->it2,
                &option_name_len);
            option_container = psg_json_value_iterator_get_value(ctx->it2);
            default_app_config = psg_json_value_get(ctx->default_app_config_container,
                option_name, option_name_len);
            if (default_app_config != NULL) {
                value_hierarchy_doc = psg_json_value_get(option_container,
                    "value_hierarchy", -1);
                value_hierarchy_from_default = psg_json_value_get(default_app_config,
                    "value_hierarchy", -1);
                psg_json_value_append_vals(value_hierarchy_doc,
                    value_hierarchy_from_default,
                    ctx->it3, ctx->end3);
                maybe_inherit_string_array_hierarchy_values(value_hierarchy_doc,
                    ctx->it3, ctx->end3);
                maybe_inherit_string_keyval_hierarchy_values(value_hierarchy_doc,
                    ctx->it3, ctx->end3);
            }

            psg_json_value_iterator_advance(ctx->it2);
        }

        /* Iterate through all 'default_application_configuration' options */
        psg_json_value_begin(ctx->default_app_config_container, ctx->it2);
        psg_json_value_end(ctx->default_app_config_container, ctx->end2);
        while (!psg_json_value_iterator_eq(ctx->it2, ctx->end2)) {
            /* For each default app config object, if there is no object in
             * the current context's 'options' with the same name, then add
             * it there.
             */
            option_name = psg_json_value_iterator_get_name(ctx->it2,
                &option_name_len);
            if (!psg_json_value_is_member(options_container, option_name, option_name_len)) {
                option_container = psg_json_value_iterator_get_value(ctx->it2);
                psg_json_value_set_value(options_container, option_name, option_name_len,
                    option_container);
            }

            psg_json_value_iterator_advance(ctx->it2);
        }

        psg_json_value_iterator_advance(ctx->it);
    }
}

static void
manifest_inherit_location_value_hierarchies(manifest_gen_ctx_t *ctx) {
    PsgJsonValue *app_config_container, *location_configs_container, *location_config_container;
    PsgJsonValue *app_default_location_configs;
    PsgJsonValue *options_container, *option_container, *default_location_config;
    PsgJsonValue *value_hierarchy_doc, *value_hierarchy_from_default;
    const char *option_name;
    size_t option_name_len;

    /* Iterate through all 'application_configurations' objects */
    psg_json_value_begin(ctx->app_configs_container, ctx->it);
    psg_json_value_end(ctx->app_configs_container, ctx->end);
    while (!psg_json_value_iterator_eq(ctx->it, ctx->end)) {
        app_config_container = psg_json_value_iterator_get_value(ctx->it);

        /* Iterate through all its 'default_location_configuration' options */
        app_default_location_configs = psg_json_value_get(app_config_container,
            "default_location_configuration", -1);
        options_container = app_default_location_configs;
        psg_json_value_begin(options_container, ctx->it2);
        psg_json_value_end(options_container, ctx->end2);
        while (!psg_json_value_iterator_eq(ctx->it2, ctx->end2)) {
            /* For each option, inherit the value hierarchies
             * from the top-level 'default_application_configuration' object.
             *
             * Since the value hierarchy array is already in
             * most-to-least-specific order, simply appending
             * the 'default_application_configuration' hierarchy is
             * enough.
             */
            option_name = psg_json_value_iterator_get_name(ctx->it2, &option_name_len);
            option_container = psg_json_value_iterator_get_value(ctx->it2);
            default_location_config = psg_json_value_get(ctx->default_loc_config_container,
                option_name, option_name_len);
            if (default_location_config != NULL) {
                value_hierarchy_doc = psg_json_value_get(option_container,
                    "value_hierarchy", -1);
                value_hierarchy_from_default = psg_json_value_get(default_location_config,
                    "value_hierarchy", -1);
                psg_json_value_append_vals(value_hierarchy_doc,
                    value_hierarchy_from_default,
                    ctx->it3, ctx->end3);
                maybe_inherit_string_array_hierarchy_values(value_hierarchy_doc,
                    ctx->it3, ctx->end3);
                maybe_inherit_string_keyval_hierarchy_values(value_hierarchy_doc,
                    ctx->it3, ctx->end3);
            }

            psg_json_value_iterator_advance(ctx->it2);
        }

        /* Iterate through all top-level 'default_location_configuration' options */
        psg_json_value_begin(ctx->default_loc_config_container, ctx->it2);
        psg_json_value_end(ctx->default_loc_config_container, ctx->end2);
        while (!psg_json_value_iterator_eq(ctx->it2, ctx->end2)) {
            /* For each default top-level 'default_location_configuration' option,
             * if there is no object in the current context's 'default_application_configuration'
             * with the same name, then add it there.
             */
            option_name = psg_json_value_iterator_get_name(ctx->it2,
                &option_name_len);
            if (!psg_json_value_is_member(options_container, option_name, option_name_len)) {
                option_container = psg_json_value_iterator_get_value(ctx->it2);
                psg_json_value_set_value(options_container, option_name, option_name_len,
                    option_container);
            }

            psg_json_value_iterator_advance(ctx->it2);
        }

        /* Iterate through all its 'locations_configurations' options */
        location_configs_container = psg_json_value_get(app_config_container,
            "location_configurations", -1);
        if (location_configs_container != NULL) {
            psg_json_value_begin(location_configs_container, ctx->it2);
            psg_json_value_end(location_configs_container, ctx->end2);
            while (!psg_json_value_iterator_eq(ctx->it2, ctx->end2)) {
                location_config_container = psg_json_value_iterator_get_value(ctx->it2);

                options_container = psg_json_value_get(location_config_container,
                    "options", -1);
                psg_json_value_begin(options_container, ctx->it3);
                psg_json_value_end(options_container, ctx->end3);
                while (!psg_json_value_iterator_eq(ctx->it3, ctx->end3)) {
                    /* For each option, inherit the value hierarchies
                     * from the 'default_location_configuration' belonging
                     * to the current app (which also contains the global
                     * location config defaults).
                     *
                     * Since the value hierarchy array is already in
                     * most-to-least-specific order, simply appending
                     * the 'default_location_configuration' hierarchy is
                     * enough.
                     */
                    option_name = psg_json_value_iterator_get_name(ctx->it3, &option_name_len);
                    option_container = psg_json_value_iterator_get_value(ctx->it3);
                    default_location_config = psg_json_value_get(app_default_location_configs,
                        option_name, option_name_len);
                    if (default_location_config != NULL) {
                        value_hierarchy_doc = psg_json_value_get(option_container,
                            "value_hierarchy", -1);
                        value_hierarchy_from_default = psg_json_value_get(default_location_config,
                            "value_hierarchy", -1);
                        psg_json_value_append_vals(value_hierarchy_doc,
                            value_hierarchy_from_default,
                            ctx->it4, ctx->end4);
                        maybe_inherit_string_array_hierarchy_values(value_hierarchy_doc,
                            ctx->it4, ctx->end4);
                        maybe_inherit_string_keyval_hierarchy_values(value_hierarchy_doc,
                            ctx->it4, ctx->end4);
                    }

                    psg_json_value_iterator_advance(ctx->it3);
                }

                psg_json_value_iterator_advance(ctx->it2);
            }
        }

        psg_json_value_iterator_advance(ctx->it);
    }
}

static void
maybe_inherit_string_array_hierarchy_values(PsgJsonValue *value_hierarchy_doc,
    PsgJsonValueIterator *it, PsgJsonValueIterator *end)
{
    PsgJsonValue *value, *current, *next, *current_value, *next_value;
    unsigned int len;
    int i;

    if (psg_json_value_size(value_hierarchy_doc) == 0) {
        return;
    }
    value = psg_json_value_get(psg_json_value_get_at_index(value_hierarchy_doc, 0),
        "value", -1);
    if (psg_json_value_type(value) != PSG_JSON_VALUE_TYPE_ARRAY) {
        return;
    }

    len = psg_json_value_size(value_hierarchy_doc);
    for (i = len - 1; i >= 1; i--) {
        current = psg_json_value_get_at_index(value_hierarchy_doc, i);
        next = psg_json_value_get_at_index(value_hierarchy_doc, i - 1);

        current_value = psg_json_value_get(current, "value", -1);
        next_value = psg_json_value_get(next, "value", -1);

        psg_json_value_begin(current_value, it);
        psg_json_value_end(current_value, end);
        while (!psg_json_value_iterator_eq(it, end)) {
            if (!json_array_contains(next_value, psg_json_value_iterator_get_value(it))) {
                psg_json_value_append_val(next_value, psg_json_value_iterator_get_value(it));
            }
            psg_json_value_iterator_advance(it);
        }
    }
}

static void
maybe_inherit_string_keyval_hierarchy_values(PsgJsonValue *value_hierarchy_doc,
    PsgJsonValueIterator *it, PsgJsonValueIterator *end)
{
    PsgJsonValue *value, *current, *next, *current_value, *next_value;
    const char *name;
    size_t name_len;
    unsigned int len;
    int i;

    if (psg_json_value_size(value_hierarchy_doc) == 0) {
        return;
    }
    value = psg_json_value_get(psg_json_value_get_at_index(value_hierarchy_doc, 0),
        "value", -1);
    if (psg_json_value_type(value) != PSG_JSON_VALUE_TYPE_OBJECT) {
        return;
    }

    len = psg_json_value_size(value_hierarchy_doc);
    for (i = len - 1; i >= 1; i--) {
        current = psg_json_value_get_at_index(value_hierarchy_doc, i);
        next = psg_json_value_get_at_index(value_hierarchy_doc, i - 1);

        current_value = psg_json_value_get(current, "value", -1);
        next_value = psg_json_value_get(next, "value", -1);

        psg_json_value_begin(current_value, it);
        psg_json_value_end(current_value, end);
        while (!psg_json_value_iterator_eq(it, end)) {
            name = psg_json_value_iterator_get_name(it, &name_len);
            if (!psg_json_value_is_member(next_value, name, name_len)) {
                psg_json_value_set_value(next_value, name, name_len,
                    psg_json_value_iterator_get_value(it));
            }
            psg_json_value_iterator_advance(it);
        }
    }
}


static void
psg_json_value_append_vals(PsgJsonValue *doc, PsgJsonValue *doc2,
    PsgJsonValueIterator *it, PsgJsonValueIterator *end)
{
    PsgJsonValue *elem;

    psg_json_value_begin(doc2, it);
    psg_json_value_end(doc2, end);
    while (!psg_json_value_iterator_eq(it, end)) {
        elem = psg_json_value_iterator_get_value(it);
        psg_json_value_append_val(doc, elem);
        psg_json_value_iterator_advance(it);
    }
}

static int
json_array_contains(PsgJsonValue *doc, PsgJsonValue *elem) {
    unsigned int i, len;
    PsgJsonValue *current;

    len = psg_json_value_size(doc);
    for (i = 0; i < len; i++) {
        current = psg_json_value_get_at_index(doc, i);
        if (psg_json_value_eq(current, elem)) {
            return 1;
        }
    }

    return 0;
}
