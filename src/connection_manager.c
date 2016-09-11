/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <qpid/dispatch/connection_manager.h>
#include <qpid/dispatch/ctools.h>
#include <qpid/dispatch/threading.h>
#include "dispatch_private.h"
#include "connection_manager_private.h"
#include "server_private.h"
#include "entity.h"
#include "entity_cache.h"
#include "schema_enum.h"
#include <string.h>
#include <stdio.h>

static char* HOST_ADDR_DEFAULT = "127.0.0.1";

struct qd_config_ssl_profile_t {
    DEQ_LINKS(qd_config_ssl_profile_t);
    uint64_t   identity;
    char      *name;
    char      *ssl_password;
    char      *ssl_trusted_certificate_db;
    char      *ssl_trusted_certificates;
    char      *ssl_uid_format;
    char      *ssl_display_name_file;
    char      *ssl_certificate_file;
    char      *ssl_private_key_file;
    int       ref_count;
};

struct qd_config_listener_t {
    bool                     is_connector;
    qd_bind_state_t          state;
    qd_listener_t           *listener;
    qd_config_ssl_profile_t *ssl_profile;
    qd_server_config_t       configuration;
    DEQ_LINKS(qd_config_listener_t);
};

DEQ_DECLARE(qd_config_listener_t, qd_config_listener_list_t);
DEQ_DECLARE(qd_config_ssl_profile_t, qd_config_ssl_profile_list_t);


struct qd_config_connector_t {
    bool is_connector;
    DEQ_LINKS(qd_config_connector_t);
    qd_connector_t          *connector;
    qd_server_config_t       configuration;
    qd_config_ssl_profile_t *ssl_profile;
};

DEQ_DECLARE(qd_config_connector_t, qd_config_connector_list_t);

struct qd_connection_manager_t {
    qd_log_source_t              *log_source;
    qd_server_t                  *server;
    qd_config_listener_list_t     config_listeners;
    qd_config_connector_list_t    config_connectors;
    qd_config_ssl_profile_list_t  config_ssl_profiles;
    sys_mutex_t                  *ssl_profile_lock;
};

/**
 * Search the list of config_ssl_profiles for an ssl-profile that matches the passed in name
 */
static qd_config_ssl_profile_t *qd_find_ssl_profile(qd_connection_manager_t *cm, char *name)
{
    qd_config_ssl_profile_t *ssl_profile = DEQ_HEAD(cm->config_ssl_profiles);
    while(ssl_profile) {
        if(strcmp(ssl_profile->name, name)==0)
            return ssl_profile;
        ssl_profile = DEQ_NEXT(ssl_profile);
    }

    return 0;
}

static void qd_server_config_free(qd_server_config_t *cf)
{
    if (!cf) return;
    free(cf->host);
    free(cf->port);
    free(cf->name);
    free(cf->role);
    free(cf->sasl_mechanisms);

    memset(cf, 0, sizeof(*cf));
}

#define CHECK() if (qd_error_code()) goto error

/**
 * Private function to set the values of booleans strip_inbound_annotations and strip_outbound_annotations
 * based on the corresponding values for the settings in qdrouter.json
 * strip_inbound_annotations and strip_outbound_annotations are defaulted to true
 */
static void load_strip_annotations(qd_server_config_t *config, const char* stripAnnotations)
{
    if (stripAnnotations) {
    	if      (strcmp(stripAnnotations, "both") == 0) {
    		config->strip_inbound_annotations  = true;
    		config->strip_outbound_annotations = true;
    	}
    	else if (strcmp(stripAnnotations, "in") == 0) {
    		config->strip_inbound_annotations  = true;
    		config->strip_outbound_annotations = false;
    	}
    	else if (strcmp(stripAnnotations, "out") == 0) {
    		config->strip_inbound_annotations  = false;
    		config->strip_outbound_annotations = true;
    	}
    	else if (strcmp(stripAnnotations, "no") == 0) {
    		config->strip_inbound_annotations  = false;
    		config->strip_outbound_annotations = false;
    	}
    }
    else {
    	assert(stripAnnotations);
    	//This is just for safety. Default to stripInboundAnnotations and stripOutboundAnnotations to true (to "both").
		config->strip_inbound_annotations  = true;
		config->strip_outbound_annotations = true;
    }
}

/**
 * Since both the host and the addr have defaults of 127.0.0.1, we will have to use the non-default wherever it is available.
 */
static void set_config_host(qd_server_config_t *config, qd_entity_t* entity)
{
    char *host = qd_entity_opt_string(entity, "host", 0);
    char *addr = qd_entity_opt_string(entity, "addr", 0);

    if (strcmp(host, HOST_ADDR_DEFAULT) == 0 && strcmp(addr, HOST_ADDR_DEFAULT) == 0) {
        config->host = host;
    }
    else if (strcmp(host, addr) == 0) {
        config->host = host;
    }
    else if (strcmp(host, HOST_ADDR_DEFAULT) == 0 && strcmp(addr, HOST_ADDR_DEFAULT) != 0) {
         config->host = addr;
    }
    else if (strcmp(host, HOST_ADDR_DEFAULT) != 0 && strcmp(addr, HOST_ADDR_DEFAULT) == 0) {
         config->host = host;
    }

    assert(config->host);
}

static qd_error_t load_server_config(qd_dispatch_t *qd, qd_server_config_t *config, qd_entity_t* entity, qd_config_ssl_profile_t **ssl_profile)
{
    qd_error_clear();

    bool authenticatePeer   = qd_entity_opt_bool(entity, "authenticatePeer",  true);    CHECK();
    bool verifyHostName     = qd_entity_opt_bool(entity, "verifyHostName",    true);     CHECK();
    char *stripAnnotations  = qd_entity_opt_string(entity, "stripAnnotations", 0);       CHECK();
    bool requireEncryption  = qd_entity_opt_bool(entity, "requireEncryption", false);    CHECK();
    bool requireSsl         = qd_entity_opt_bool(entity, "requireSsl",        false);    CHECK();
    bool depRequirePeerAuth = qd_entity_opt_bool(entity, "requirePeerAuth",   false);    CHECK();
    bool depAllowUnsecured  = qd_entity_opt_bool(entity, "allowUnsecured", !requireSsl); CHECK();

    memset(config, 0, sizeof(*config));
    config->port                 = qd_entity_get_string(entity, "port");              CHECK();
    config->name                 = qd_entity_opt_string(entity, "name", 0);           CHECK();
    config->role                 = qd_entity_get_string(entity, "role");              CHECK();
    config->inter_router_cost    = qd_entity_opt_long(entity, "cost", 1);             CHECK();
    config->protocol_family      = qd_entity_opt_string(entity, "protocolFamily", 0); CHECK();
    config->max_frame_size       = qd_entity_get_long(entity, "maxFrameSize");        CHECK();
    config->idle_timeout_seconds = qd_entity_get_long(entity, "idleTimeoutSeconds");  CHECK();
    config->sasl_username        = qd_entity_opt_string(entity, "saslUsername", 0);   CHECK();
    config->sasl_password        = qd_entity_opt_string(entity, "saslPassword", 0);   CHECK();
    config->sasl_mechanisms      = qd_entity_opt_string(entity, "saslMechanisms", 0); CHECK();
    config->ssl_profile          = qd_entity_opt_string(entity, "sslProfile", 0);     CHECK();
    config->link_capacity        = qd_entity_opt_long(entity, "linkCapacity", 0);     CHECK();
    set_config_host(config, entity);

    //
    // Handle the defaults for link capacity.
    //
    if (config->link_capacity == 0)
        config->link_capacity = 250;

    //
    // For now we are hardwiring this attribute to true.  If there's an outcry from the
    // user community, we can revisit this later.
    //
    config->allowInsecureAuthentication = true;
    config->verify_host_name = verifyHostName;

    load_strip_annotations(config, stripAnnotations);

    config->requireAuthentication = authenticatePeer || depRequirePeerAuth;
    config->requireEncryption     = requireEncryption || !depAllowUnsecured;


    if (config->ssl_profile) {
        config->ssl_required = requireSsl || !depAllowUnsecured;
        config->ssl_require_peer_authentication = config->sasl_mechanisms &&
            strstr(config->sasl_mechanisms, "EXTERNAL") != 0;

        *ssl_profile = qd_find_ssl_profile(qd->connection_manager, config->ssl_profile);
        if(*ssl_profile) {
            config->ssl_certificate_file = (*ssl_profile)->ssl_certificate_file;
            config->ssl_private_key_file = (*ssl_profile)->ssl_private_key_file;
            config->ssl_password = (*ssl_profile)->ssl_password;
            config->ssl_trusted_certificate_db = (*ssl_profile)->ssl_trusted_certificate_db;
            config->ssl_trusted_certificates = (*ssl_profile)->ssl_trusted_certificates;
            config->ssl_uid_format = (*ssl_profile)->ssl_uid_format;
            config->ssl_display_name_file = (*ssl_profile)->ssl_display_name_file;
        }
        sys_mutex_lock(qd->connection_manager->ssl_profile_lock);
        (*ssl_profile)->ref_count++;
        sys_mutex_unlock(qd->connection_manager->ssl_profile_lock);
    }

    free(stripAnnotations);
    return QD_ERROR_NONE;

  error:
    qd_server_config_free(config);
    return qd_error_code();
}


qd_config_ssl_profile_t *qd_dispatch_configure_ssl_profile(qd_dispatch_t *qd, qd_entity_t *entity)
{
    qd_error_clear();
    qd_connection_manager_t *cm = qd->connection_manager;

    qd_config_ssl_profile_t *ssl_profile = NEW(qd_config_ssl_profile_t);
    DEQ_ITEM_INIT(ssl_profile);
    DEQ_INSERT_TAIL(cm->config_ssl_profiles, ssl_profile);
    ssl_profile->name                       = qd_entity_opt_string(entity, "name", 0); CHECK();
    ssl_profile->ssl_certificate_file       = qd_entity_opt_string(entity, "certFile", 0); CHECK();
    ssl_profile->ssl_private_key_file       = qd_entity_opt_string(entity, "keyFile", 0); CHECK();
    ssl_profile->ssl_password               = qd_entity_opt_string(entity, "password", 0); CHECK();
    ssl_profile->ssl_trusted_certificate_db = qd_entity_opt_string(entity, "certDb", 0); CHECK();
    ssl_profile->ssl_trusted_certificates   = qd_entity_opt_string(entity, "trustedCerts", 0); CHECK();
    ssl_profile->ssl_uid_format             = qd_entity_opt_string(entity, "uidFormat", 0); CHECK();
    ssl_profile->ssl_display_name_file      = qd_entity_opt_string(entity, "displayNameFile", 0); CHECK();
    sys_mutex_lock(qd->connection_manager->ssl_profile_lock);
    ssl_profile->ref_count                  = 0;
    sys_mutex_unlock(qd->connection_manager->ssl_profile_lock);
    qd_log(cm->log_source, QD_LOG_INFO, "Created SSL Profile with name %s ", ssl_profile->name);
    return ssl_profile;

    error:
        qd_log(cm->log_source, QD_LOG_ERROR, "Unable to create ssl profile: %s", qd_error_message());
        free(ssl_profile);
        return 0;
}


qd_config_listener_t *qd_dispatch_configure_listener(qd_dispatch_t *qd, qd_entity_t *entity)
{
    qd_error_clear();
    qd_connection_manager_t *cm = qd->connection_manager;
    qd_config_listener_t *cl = NEW(qd_config_listener_t);
    cl->is_connector = false;
    cl->state = QD_BIND_NONE;
    cl->listener = 0;
    qd_config_ssl_profile_t *ssl_profile = 0;
    if (load_server_config(qd, &cl->configuration, entity, &ssl_profile) != QD_ERROR_NONE) {
        qd_log(cm->log_source, QD_LOG_ERROR, "Unable to create config listener: %s", qd_error_message());
        qd_config_listener_free(qd->connection_manager, cl);
        return 0;
    }
    cl->ssl_profile = ssl_profile;
    DEQ_ITEM_INIT(cl);
    DEQ_INSERT_TAIL(cm->config_listeners, cl);

    qd_log(cm->log_source, QD_LOG_INFO, "Configured Listener: %s:%s proto=%s, role=%s%s%s",
           cl->configuration.host, cl->configuration.port,
           cl->configuration.protocol_family ? cl->configuration.protocol_family : "any",
           cl->configuration.role,
           cl->ssl_profile ? ", sslProfile=":"",
           cl->ssl_profile ? cl->ssl_profile->name:"");

    return cl;
}


qd_error_t qd_entity_refresh_listener(qd_entity_t* entity, void *impl)
{
    return QD_ERROR_NONE;
}


qd_error_t qd_entity_refresh_connector(qd_entity_t* entity, void *impl)
{
    return QD_ERROR_NONE;
}


qd_config_connector_t *qd_dispatch_configure_connector(qd_dispatch_t *qd, qd_entity_t *entity)
{
    qd_error_clear();
    qd_connection_manager_t *cm = qd->connection_manager;
    qd_config_connector_t *cc = NEW(qd_config_connector_t);
    ZERO(cc);

    cc->is_connector = true;
    qd_config_ssl_profile_t *ssl_profile = 0;
    if (load_server_config(qd, &cc->configuration, entity, &ssl_profile) != QD_ERROR_NONE) {
        qd_log(cm->log_source, QD_LOG_ERROR, "Unable to create config connector: %s", qd_error_message());
        qd_config_connector_free(qd->connection_manager, cc);
        return 0;
    }
    cc->ssl_profile = ssl_profile;
    DEQ_ITEM_INIT(cc);
    DEQ_INSERT_TAIL(cm->config_connectors, cc);
    qd_log(cm->log_source, QD_LOG_INFO, "Configured Connector: %s:%s proto=%s, role=%s %s%s",
            cc->configuration.host, cc->configuration.port,
            cc->configuration.protocol_family ? cc->configuration.protocol_family : "any",
            cc->configuration.role,
            cc->ssl_profile ? ", sslProfile=":"",
            cc->ssl_profile ? cc->ssl_profile->name:"");

    return cc;
}


qd_connection_manager_t *qd_connection_manager(qd_dispatch_t *qd)
{
    qd_connection_manager_t *cm = NEW(qd_connection_manager_t);
    if (!cm)
        return 0;

    cm->log_source = qd_log_source("CONN_MGR");
    cm->ssl_profile_lock = sys_mutex();
    cm->server     = qd->server;
    DEQ_INIT(cm->config_listeners);
    DEQ_INIT(cm->config_connectors);
    DEQ_INIT(cm->config_ssl_profiles);

    return cm;
}


void qd_connection_manager_free(qd_connection_manager_t *cm)
{
    if (!cm) return;
    qd_config_listener_t *cl = DEQ_HEAD(cm->config_listeners);
    while (cl) {
        DEQ_REMOVE_HEAD(cm->config_listeners);
        qd_server_listener_free(cl->listener);
        qd_server_config_free(&cl->configuration);
        free(cl);
        cl = DEQ_HEAD(cm->config_listeners);
    }

    qd_config_connector_t *cc = DEQ_HEAD(cm->config_connectors);
    while (cc) {
        DEQ_REMOVE_HEAD(cm->config_connectors);
        qd_server_connector_free(cc->connector);
        qd_server_config_free(&cc->configuration);
        free(cc);
        cc = DEQ_HEAD(cm->config_connectors);
    }

    qd_config_ssl_profile_t *sslp = DEQ_HEAD(cm->config_ssl_profiles);
    while (sslp) {
        DEQ_REMOVE_HEAD(cm->config_ssl_profiles);
        qd_config_ssl_profile_free(cm, sslp);
        sslp = DEQ_HEAD(cm->config_ssl_profiles);
    }

    sys_mutex_free(cm->ssl_profile_lock);
}


void qd_connection_manager_start(qd_dispatch_t *qd)
{
    qd_config_listener_t  *cl = DEQ_HEAD(qd->connection_manager->config_listeners);
    qd_config_connector_t *cc = DEQ_HEAD(qd->connection_manager->config_connectors);

    while (cl) {
        if (cl->listener == 0 )
            if (cl->state == QD_BIND_NONE) { //Try to start listening only if we have never tried to listen on that port before
                cl->listener = qd_server_listen(qd, &cl->configuration, cl);
                if (cl->listener && cl->listener->pn_listener)
                    cl->state = QD_BIND_SUCCESSFUL;
                else
                    cl->state = QD_BIND_FAILED;
            }
        cl = DEQ_NEXT(cl);
    }

    while (cc) {
        if (cc->connector == 0)
            cc->connector = qd_server_connect(qd, &cc->configuration, cc);
        cc = DEQ_NEXT(cc);
    }
}


void qd_config_connector_free(qd_connection_manager_t *cm, qd_config_connector_t *cc)
{
    if (cc->connector)
        qd_server_connector_free(cc->connector);

    if (cc->ssl_profile) {
        sys_mutex_lock(cm->ssl_profile_lock);
        cc->ssl_profile->ref_count--;
        sys_mutex_unlock(cm->ssl_profile_lock);
    }

    free(cc);
}


void qd_config_listener_free(qd_connection_manager_t *cm, qd_config_listener_t *cl)
{
    if (cl->listener) {
        qd_server_listener_close(cl->listener);
        qd_server_listener_free(cl->listener);
        cl->listener = 0;
    }

    if (cl->ssl_profile) {
        sys_mutex_lock(cm->ssl_profile_lock);
        cl->ssl_profile->ref_count--;
        sys_mutex_unlock(cm->ssl_profile_lock);
    }

    free(cl);
}


bool qd_config_ssl_profile_free(qd_connection_manager_t *cm, qd_config_ssl_profile_t *ssl_profile)
{
    sys_mutex_lock(cm->ssl_profile_lock);
    if (ssl_profile->ref_count != 0) {
        sys_mutex_unlock(cm->ssl_profile_lock);
        return false;
    }
    sys_mutex_unlock(cm->ssl_profile_lock);

    DEQ_REMOVE(cm->config_ssl_profiles, ssl_profile);

    free(ssl_profile->name);
    free(ssl_profile->ssl_password);
    free(ssl_profile->ssl_trusted_certificate_db);
    free(ssl_profile->ssl_trusted_certificates);
    free(ssl_profile->ssl_uid_format);
    free(ssl_profile->ssl_display_name_file);
    free(ssl_profile->ssl_certificate_file);
    free(ssl_profile->ssl_private_key_file);
    free(ssl_profile);
    return true;

}


void qd_connection_manager_delete_listener(qd_dispatch_t *qd, void *impl)
{
    qd_config_listener_t *cl = (qd_config_listener_t*) impl;

    if (cl) {
        qd_server_listener_close(cl->listener);
        DEQ_REMOVE(qd->connection_manager->config_listeners, cl);
        qd_config_listener_free(qd->connection_manager, cl);
    }
}


/**
 * Only those SSL Profiles that are not being referenced from other listeners/connectors can be deleted
 */
bool qd_connection_manager_delete_ssl_profile(qd_dispatch_t *qd, void *impl)
{
    qd_config_ssl_profile_t *ssl_profile = (qd_config_ssl_profile_t*) impl;
    if(ssl_profile) {
        bool freed = qd_config_ssl_profile_free(qd->connection_manager, ssl_profile);

        return freed;
    }
    return false;
}


void qd_connection_manager_delete_connector(qd_dispatch_t *qd, void *impl)
{
    qd_config_connector_t *cc = (qd_config_connector_t*) impl;

    if (cc) {
        DEQ_REMOVE(qd->connection_manager->config_connectors, cc);
        qd_config_connector_free(qd->connection_manager, cc);
    }
}


const char *qd_config_connector_name(qd_config_connector_t *cc)
{
    return cc ? cc->configuration.name : 0;
}

