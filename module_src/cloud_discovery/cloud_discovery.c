/*
** Copyright (C) 2014 Daisuke Ikeda
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/

#include "sysinc.h"
#include "module.h"
#include "zbxjson.h"
#include "ipc.h"
#include "memalloc.h"
#include "log.h"
#include "zbxalgo.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <libdeltacloud/libdeltacloud.h>

#define ZBX_IPC_CLOUD_ID 'c'
#define NAME_MACRO "{#INSTANCE.NAME}"
#define ID_MACRO "{#INSTANCE.ID}"
#define PUBLIC_ADDR_MACRO "{#INSTANCE.PUBLIC_ADDR}"
#define PRIVATE_ADDR_MACRO "{#INSTANCE.PRIVATE_ADDR}"
#define CONFIG_FILE "/usr/local/zabbix/2.1.7/etc/zabbix_agentd.conf"
#define MEM_SIZE 1048576
#define EXPIRE_TIME 60*60*24

/* the variable keeps timeout setting for item processing */
static int	item_timeout = 0;

int	zbx_module_cloud_discovery(AGENT_REQUEST *request, AGENT_RESULT *result);
int	zbx_module_cloud_monitor(AGENT_REQUEST *request, AGENT_RESULT *result);
int	zbx_module_cloud_instance_list(AGENT_REQUEST *request, AGENT_RESULT *result);
int	zbx_module_cloud_instance_status(AGENT_REQUEST *request, AGENT_RESULT *result);
int	zbx_module_cloud_instance_owner_id(AGENT_REQUEST *request, AGENT_RESULT *result);
int	zbx_module_cloud_instance_image_id(AGENT_REQUEST *request, AGENT_RESULT *result);
int	zbx_module_cloud_instance_image_href(AGENT_REQUEST *request, AGENT_RESULT *result);
int	zbx_module_cloud_instance_realm_id(AGENT_REQUEST *request, AGENT_RESULT *result);
int	zbx_module_cloud_instance_realm_href(AGENT_REQUEST *request, AGENT_RESULT *result);
int	zbx_module_cloud_instance_launch_time(AGENT_REQUEST *request, AGENT_RESULT *result);
int	zbx_module_cloud_instance_hwp_href(AGENT_REQUEST *request, AGENT_RESULT *result);
int	zbx_module_cloud_instance_hwp_id(AGENT_REQUEST *request, AGENT_RESULT *result);
int	zbx_module_cloud_instance_hwp_name(AGENT_REQUEST *request, AGENT_RESULT *result);

static zbx_mem_info_t   *cloud_mem = NULL;

ZBX_MEM_FUNC_IMPL(__cloud, cloud_mem);

//////


typedef struct
{
	zbx_vector_ptr_t	services;
}
zbx_deltacloud_t;

typedef struct
{
	char    *url;
        char    *key;
        char    *secret;
        char    *driver;
        char    *provider;
        int	lastcheck;
        int	lastaccess;
        zbx_vector_ptr_t  instances;
}
zbx_deltacloud_service_t;


typedef struct deltacloud_hardware_profile zbx_deltacloud_hardware_profile_t;

typedef struct
{
	char *href;
	char *id;
	char *name;
	char *owner_id;
	char *image_id;
	char *image_href;
	char *realm_id;
	char *realm_href;
	char *state;
	char *launch_time;
	zbx_deltacloud_hardware_profile_t *hwp;
	zbx_vector_ptr_t public_addresses;
	zbx_vector_ptr_t private_addresses;
}
zbx_deltacloud_instance_t;

typedef struct deltacloud_address zbx_deltacloud_address_t;

static zbx_deltacloud_t	*deltacloud = NULL; 
static void     cloud_service_shared_free(zbx_deltacloud_service_t *service);
static void	cloud_instance_shared_free(zbx_deltacloud_instance_t *instance);

#define CLOUD_VECTOR_CREATE(ref, type) zbx_vector_##type##_create_ext(ref, __cloud_mem_malloc_func, __cloud_mem_realloc_func, __cloud_mem_free_func)

///////

static ZBX_METRIC keys[] =
/*      KEY                     FLAG		FUNCTION        	TEST PARAMETERS */
{
	{"cloud.monitor",	CF_HAVEPARAMS,	zbx_module_cloud_monitor,"http://hostname/api,ABC1223DE,ZDADQWQ2133"},
	{"cloud.instance.list",	CF_HAVEPARAMS,	zbx_module_cloud_instance_list,"http://hostname/api,ABC1223DE,ZDADQWQ2133"},
	{"cloud.instance.status",	CF_HAVEPARAMS,	zbx_module_cloud_instance_status,"http://hostname/api,ABC1223DE,ZDADQWQ2133, instance_id"},
	{"cloud.instance.owner_id",	CF_HAVEPARAMS,	zbx_module_cloud_instance_owner_id,"http://hostname/api,ABC1223DE,ZDADQWQ2133, instance_id"},
	{"cloud.instance.image_id",	CF_HAVEPARAMS,	zbx_module_cloud_instance_image_id,"http://hostname/api,ABC1223DE,ZDADQWQ2133, instance_id"},
	{"cloud.instance.image_href",	CF_HAVEPARAMS,	zbx_module_cloud_instance_image_href,"http://hostname/api,ABC1223DE,ZDADQWQ2133, instance_id"},
	{"cloud.instance.realm_id",	CF_HAVEPARAMS,	zbx_module_cloud_instance_realm_id,"http://hostname/api,ABC1223DE,ZDADQWQ2133, instance_id"},
	{"cloud.instance.realm_href",	CF_HAVEPARAMS,	zbx_module_cloud_instance_realm_href,"http://hostname/api,ABC1223DE,ZDADQWQ2133, instance_id"},
	{"cloud.instance.launch_time",	CF_HAVEPARAMS,	zbx_module_cloud_instance_launch_time,"http://hostname/api,ABC1223DE,ZDADQWQ2133, instance_id"},
	{"cloud.instance.hwp.href",	CF_HAVEPARAMS,	zbx_module_cloud_instance_hwp_href,"http://hostname/api,ABC1223DE,ZDADQWQ2133, instance_id"},
	{"cloud.instance.hwp.id",	CF_HAVEPARAMS,	zbx_module_cloud_instance_hwp_id,"http://hostname/api,ABC1223DE,ZDADQWQ2133, instance_id"},
	{"cloud.instance.hwp.name",	CF_HAVEPARAMS,	zbx_module_cloud_instance_hwp_name,"http://hostname/api,ABC1223DE,ZDADQWQ2133, instance_id"},
	{NULL}
};

/******************************************************************************
 *                                                                            *
 * Function: zbx_module_api_version                                           *
 *                                                                            *
 * Purpose: returns version number of the module interface                    *
 *                                                                            *
 * Return value: ZBX_MODULE_API_VERSION_ONE - the only version supported by   *
 *               Zabbix currently                                             *
 *                                                                            *
 ******************************************************************************/
int	zbx_module_api_version()
{
	return ZBX_MODULE_API_VERSION_ONE;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_module_item_timeout                                          *
 *                                                                            *
 * Purpose: set timeout value for processing of items                         *
 *                                                                            *
 * Parameters: timeout - timeout in seconds, 0 - no timeout set               *
 *                                                                            *
 ******************************************************************************/
void	zbx_module_item_timeout(int timeout)
{
	item_timeout = timeout;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_module_item_list                                             *
 *                                                                            *
 * Purpose: returns list of item keys supported by the module                 *
 *                                                                            *
 * Return value: list of item keys                                            *
 *                                                                            *
 ******************************************************************************/
ZBX_METRIC	*zbx_module_item_list()
{
	return keys;
}


static char	*cloud_shared_strdup(const char *source)
{
	char	*ptr = NULL;
	size_t	len;

	if (NULL != source)
	{
		len = strlen(source) + 1;
		ptr = __cloud_mem_malloc_func(NULL, len);
		memcpy(ptr, source, len);
	}

	return ptr;
}

static zbx_deltacloud_hardware_profile_t *cloud_hardware_profile_shared_dup(const struct deltacloud_hardware_profile *src)
{
	zbx_deltacloud_hardware_profile_t	*hardware_profile;
	hardware_profile = __cloud_mem_malloc_func(NULL, sizeof(zbx_deltacloud_hardware_profile_t));
	
	hardware_profile->href = cloud_shared_strdup(src->href);
	hardware_profile->id = cloud_shared_strdup(src->id);
	hardware_profile->name = cloud_shared_strdup(src->name);
	return hardware_profile;
}
	

zbx_deltacloud_service_t	*zbx_deltacloud_get_service(const char* url, const char* key, const char* secret, const char* driver, const char* provider)
{
	int i;
	zbx_deltacloud_service_t	*service = NULL;

	if (NULL == deltacloud)
	{
		zabbix_log(LOG_LEVEL_ERR, "---Not initialized shared memory---");
		return NULL;
	}

	for (i = 0; i < deltacloud->services.values_num; i++)
	{
		service = deltacloud->services.values[i];
		if (0 == strcmp(service->url, url) && 0 == strcmp(service->key, key) && 0 == strcmp(service->secret, secret) && 0 == strcmp(service->driver, driver) && 0 == strcmp(service->provider, provider))
		{
			return service;
		}
	}

	service = __cloud_mem_malloc_func(NULL, sizeof(zbx_deltacloud_service_t));

	memset(service, 0, sizeof(zbx_deltacloud_service_t));

	service->url = cloud_shared_strdup(url);
	service->key = cloud_shared_strdup(key);
	service->secret = cloud_shared_strdup(secret);
	service->driver = cloud_shared_strdup(driver);
	service->provider = cloud_shared_strdup(provider);
	service->lastaccess = time(NULL);
	service->lastcheck = time(NULL);
	CLOUD_VECTOR_CREATE(&service->instances, ptr);

	zbx_vector_ptr_append(&deltacloud->services, service);
	return service;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_module_cloud_instance_list                                   *
 *                                                                            *
 * Purpose: Discovering cloud instances list from deltacloud                  *
 *                                                                            *
 * Parameters: request - structure that contains item key and parameters      *
 *              request->key - item key without parameters                    *
 *              request->nparam - number of parameters                        *
 *              request->timeout - processing should not take longer than     *
 *                                 this number of seconds                     *
 *              request->params[N-1] - pointers to item key parameters        *
 *                                                                            *
 *             result - structure that will contain result                    *
 *                                                                            *
 * Return value: SYSINFO_RET_FAIL - function failed, item will be marked      *
 *                                 as not supported by zabbix                 *
 *               SYSINFO_RET_OK - success                                     *
 *                                                                            *
 * Comment: get_rparam(request, N-1) can be used to get a pointer to the Nth  *
 *          parameter starting from 0 (first parameter). Make sure it exists  *
 *          by checking value of request->nparam.                             *
 *                                                                            *
 ******************************************************************************/
int	zbx_module_cloud_instance_list(AGENT_REQUEST *request, AGENT_RESULT *result)
{
	int i,j;
	struct zbx_json json;
	char	*url;
	char	*key;
	char	*secret;
	char	*driver;
	char	*provider;
	zbx_deltacloud_service_t	*service = NULL;

	if (request->nparam != 5)
	{
		/* set optional error message */
		SET_MSG_RESULT(result, strdup("Invalid number of parameters e.g.) cloud.instance.list[url, key, secret, driver, provider]"));
		return SYSINFO_RET_FAIL;
	}
	url = get_rparam(request, 0);
	key = get_rparam(request, 1);
	secret = get_rparam(request, 2);
	driver = get_rparam(request, 3);
	provider = get_rparam(request, 4);

	service = zbx_deltacloud_get_service(url, key, secret, driver, provider);

	if(&service->instances==NULL){
		SET_MSG_RESULT(result, strdup("No instances"));
		return SYSINFO_RET_OK;
	}
	
	// json format init
	zbx_json_init(&json, ZBX_JSON_STAT_BUF_LEN);
	// Add "data":[] for LLD format
	zbx_json_addarray(&json, ZBX_PROTO_TAG_DATA);
	for (i = 0; service->instances.values_num; i++)
	{
		//deltacloud_instance = NULL;
		zbx_deltacloud_instance_t *instance = service->instances.values[i];
		if (NULL == instance)
		{
			zbx_json_close(&json);
			break;
		}
		zbx_json_addobject(&json, NULL);
		if (NULL != instance->name)
			zbx_json_addstring(&json, NAME_MACRO, instance->name, ZBX_JSON_TYPE_STRING);
		if (NULL != instance->id)
			zbx_json_addstring(&json, ID_MACRO, instance->id, ZBX_JSON_TYPE_STRING);
		for (j = 0; instance->public_addresses.values_num; j++)
		{
			zbx_deltacloud_address_t *address = instance->public_addresses.values[j];
			zbx_json_addstring(&json, PUBLIC_ADDR_MACRO, address->address, ZBX_JSON_TYPE_STRING);
			break; /* ToDo: multi address support */
		}
		
		for (j = 0; instance->private_addresses.values_num; j++)
		{
			zbx_deltacloud_address_t *address = instance->private_addresses.values[j];
			zbx_json_addstring(&json, PRIVATE_ADDR_MACRO, address->address, ZBX_JSON_TYPE_STRING);
			break; /* ToDo: multi address support */
		}
		zbx_json_close(&json);
	}

	SET_STR_RESULT(result, strdup(json.buffer));
	zbx_json_free(&json);
	
	return SYSINFO_RET_OK;
}
	
int	zbx_module_cloud_monitor(AGENT_REQUEST *request, AGENT_RESULT *result)
{
	int i;
	char	*url;
	char	*key;
	char	*secret;
	char	*driver;
	char	*provider;
	zbx_deltacloud_service_t	*service = NULL;
	zbx_deltacloud_instance_t	*deltacloud_instance = NULL;
	zbx_deltacloud_address_t	*public_address = NULL;
	zbx_deltacloud_address_t	*private_address = NULL;
	struct deltacloud_api api;
	struct deltacloud_instance *instance = NULL;

	if (request->nparam != 5)
	{
		/* set optional error message */
		SET_MSG_RESULT(result, strdup("Invalid number of parameters e.g.) cloud.monitor[url, key, secret, driver, provider]"));
		return SYSINFO_RET_FAIL;
	}
	url = get_rparam(request, 0);
	key = get_rparam(request, 1);
	secret = get_rparam(request, 2);
	driver = get_rparam(request, 3);
	provider = get_rparam(request, 4);

	service = zbx_deltacloud_get_service(url, key, secret, driver, provider);
	zbx_vector_ptr_clean(&service->instances, (zbx_mem_free_func_t)cloud_instance_shared_free);
	deltacloud_initialize(&api, url, key, secret, driver, provider);

	deltacloud_get_instances(&api, &instance);

	if(instance==NULL){
		SET_UI64_RESULT(result, 0);
		return SYSINFO_RET_OK;
	}
	
	while(1){
		deltacloud_instance = __cloud_mem_malloc_func(NULL, sizeof(zbx_deltacloud_instance_t));
		zabbix_log(LOG_LEVEL_ERR, "-------used_size: %d---\n", cloud_mem->used_size);
		deltacloud_instance->href = cloud_shared_strdup(instance->href);
		deltacloud_instance->id = cloud_shared_strdup(instance->id);
		deltacloud_instance->name = cloud_shared_strdup(instance->name);
		deltacloud_instance->owner_id = cloud_shared_strdup(instance->owner_id);
		deltacloud_instance->image_id = cloud_shared_strdup(instance->image_id);
		deltacloud_instance->image_href = cloud_shared_strdup(instance->image_href);
		deltacloud_instance->realm_id = cloud_shared_strdup(instance->realm_id);
		deltacloud_instance->realm_href = cloud_shared_strdup(instance->realm_href);
		deltacloud_instance->state = cloud_shared_strdup(instance->state);
		deltacloud_instance->launch_time = cloud_shared_strdup(instance->launch_time);

		/* Add IP address information */
		CLOUD_VECTOR_CREATE(&deltacloud_instance->public_addresses, ptr);
		CLOUD_VECTOR_CREATE(&deltacloud_instance->private_addresses, ptr);
		public_address = __cloud_mem_malloc_func(NULL, sizeof(zbx_deltacloud_address_t));
		private_address = __cloud_mem_malloc_func(NULL, sizeof(zbx_deltacloud_address_t));

		if(instance->public_addresses)
			public_address->address = cloud_shared_strdup(instance->public_addresses->address);
		if(instance->private_addresses)
			private_address->address = cloud_shared_strdup(instance->private_addresses->address);

		
		zbx_vector_ptr_append(&deltacloud_instance->public_addresses, public_address);
		zbx_vector_ptr_append(&deltacloud_instance->private_addresses, private_address);

		deltacloud_instance->hwp = cloud_hardware_profile_shared_dup(&instance->hwp);
		zbx_vector_ptr_append(&service->instances, deltacloud_instance);
		zabbix_log(LOG_LEVEL_ERR, "-------used_size: %d---\n", cloud_mem->used_size);
		instance = instance->next;
		if(instance == NULL){
			break;
		}
	}

	SET_UI64_RESULT(result, 1);
	return SYSINFO_RET_OK;
}

int	zbx_module_cloud_instance_status(AGENT_REQUEST *request, AGENT_RESULT *result)
{

	int	i;
	char	*url;
	char	*key;
	char	*secret;
	char	*driver;
	char	*provider;
	char	*instance_id;
	
	zbx_deltacloud_service_t	*service = NULL;
	zbx_deltacloud_instance_t	*deltacloud_instance = NULL;

	if (request->nparam != 6)
	{
		/* set optional error message */
		SET_MSG_RESULT(result, strdup("Invalid number of parameters e.g.) cloud.instane.status[url, key, secret, driver, provider, instance_id]"));
		return SYSINFO_RET_FAIL;
	}
	url = get_rparam(request, 0);
	key = get_rparam(request, 1);
	secret = get_rparam(request, 2);
	driver = get_rparam(request, 3);
	provider = get_rparam(request, 4);
	instance_id = get_rparam(request, 5);

	service = zbx_deltacloud_get_service(url, key, secret, driver, provider);

	if (service == NULL)
	{
		SET_MSG_RESULT(result, strdup("No Data"));
		return SYSINFO_RET_FAIL;
	}
	
	for (i = 0; service->instances.values_num; i++)
	{
		zbx_deltacloud_instance_t *instance = service->instances.values[i];
		if (0 == strcmp(instance->id, instance_id))
		{
			SET_STR_RESULT(result, strdup(instance->state));
			return SYSINFO_RET_OK;
		}
	}
	SET_MSG_RESULT(result, strdup("Not match data"));
	return SYSINFO_RET_FAIL;
}

int	zbx_module_cloud_instance_image_id(AGENT_REQUEST *request, AGENT_RESULT *result)
{

	int	i;
	char	*url;
	char	*key;
	char	*secret;
	char	*driver;
	char	*provider;
	char	*instance_id;
	
	zbx_deltacloud_service_t	*service = NULL;
	zbx_deltacloud_instance_t	*deltacloud_instance = NULL;

	if (request->nparam != 6)
	{
		/* set optional error message */
		SET_MSG_RESULT(result, strdup("Invalid number of parameters e.g.) cloud.instane.image_id[url, key, secret, driver, provider, instance_id]"));
		return SYSINFO_RET_FAIL;
	}
	url = get_rparam(request, 0);
	key = get_rparam(request, 1);
	secret = get_rparam(request, 2);
	driver = get_rparam(request, 3);
	provider = get_rparam(request, 4);
	instance_id = get_rparam(request, 5);

	service = zbx_deltacloud_get_service(url, key, secret, driver, provider);

	if (service == NULL)
	{
		SET_MSG_RESULT(result, strdup("No Data"));
		return SYSINFO_RET_FAIL;
	}
	
	for (i = 0; service->instances.values_num; i++)
	{
		zbx_deltacloud_instance_t *instance = service->instances.values[i];
		if (0 == strcmp(instance->id, instance_id))
		{
			SET_STR_RESULT(result, strdup(instance->image_id));
			return SYSINFO_RET_OK;
		}
	}
	SET_MSG_RESULT(result, strdup("Not match data"));
	return SYSINFO_RET_FAIL;
}
int	zbx_module_cloud_instance_owner_id(AGENT_REQUEST *request, AGENT_RESULT *result)
{

	int	i;
	char	*url;
	char	*key;
	char	*secret;
	char	*driver;
	char	*provider;
	char	*instance_id;
	
	zbx_deltacloud_service_t	*service = NULL;
	zbx_deltacloud_instance_t	*deltacloud_instance = NULL;

	if (request->nparam != 6)
	{
		/* set optional error message */
		SET_MSG_RESULT(result, strdup("Invalid number of parameters e.g.) cloud.instane.owner_id[url, key, secret, driver, provider, instance_id]"));
		return SYSINFO_RET_FAIL;
	}
	url = get_rparam(request, 0);
	key = get_rparam(request, 1);
	secret = get_rparam(request, 2);
	driver = get_rparam(request, 3);
	provider = get_rparam(request, 4);
	instance_id = get_rparam(request, 5);

	service = zbx_deltacloud_get_service(url, key, secret, driver, provider);

	if (service == NULL)
	{
		SET_MSG_RESULT(result, strdup("No Data"));
		return SYSINFO_RET_FAIL;
	}
	
	for (i = 0; service->instances.values_num; i++)
	{
		zbx_deltacloud_instance_t *instance = service->instances.values[i];
		if (0 == strcmp(instance->id, instance_id))
		{
			SET_STR_RESULT(result, strdup(instance->owner_id));
			return SYSINFO_RET_OK;
		}
	}
	SET_MSG_RESULT(result, strdup("Not match data"));
	return SYSINFO_RET_FAIL;
}
int	zbx_module_cloud_instance_image_href(AGENT_REQUEST *request, AGENT_RESULT *result)
{

	int	i;
	char	*url;
	char	*key;
	char	*secret;
	char	*driver;
	char	*provider;
	char	*instance_id;
	
	zbx_deltacloud_service_t	*service = NULL;
	zbx_deltacloud_instance_t	*deltacloud_instance = NULL;

	if (request->nparam != 6)
	{
		/* set optional error message */
		SET_MSG_RESULT(result, strdup("Invalid number of parameters e.g.) cloud.instane.image_href[url, key, secret, driver, provider, instance_id]"));
		return SYSINFO_RET_FAIL;
	}
	url = get_rparam(request, 0);
	key = get_rparam(request, 1);
	secret = get_rparam(request, 2);
	driver = get_rparam(request, 3);
	provider = get_rparam(request, 4);
	instance_id = get_rparam(request, 5);

	service = zbx_deltacloud_get_service(url, key, secret, driver, provider);

	if (service == NULL)
	{
		SET_MSG_RESULT(result, strdup("No Data"));
		return SYSINFO_RET_FAIL;
	}
	
	for (i = 0; service->instances.values_num; i++)
	{
		zbx_deltacloud_instance_t *instance = service->instances.values[i];
		if (0 == strcmp(instance->id, instance_id))
		{
			SET_STR_RESULT(result, strdup(instance->image_href));
			return SYSINFO_RET_OK;
		}
	}
	SET_MSG_RESULT(result, strdup("Not match data"));
	return SYSINFO_RET_FAIL;
}
int	zbx_module_cloud_instance_realm_id(AGENT_REQUEST *request, AGENT_RESULT *result)
{

	int	i;
	char	*url;
	char	*key;
	char	*secret;
	char	*driver;
	char	*provider;
	char	*instance_id;
	
	zbx_deltacloud_service_t	*service = NULL;
	zbx_deltacloud_instance_t	*deltacloud_instance = NULL;

	if (request->nparam != 6)
	{
		/* set optional error message */
		SET_MSG_RESULT(result, strdup("Invalid number of parameters e.g.) cloud.instane.realm_id[url, key, secret, driver, provider, instance_id]"));
		return SYSINFO_RET_FAIL;
	}
	url = get_rparam(request, 0);
	key = get_rparam(request, 1);
	secret = get_rparam(request, 2);
	driver = get_rparam(request, 3);
	provider = get_rparam(request, 4);
	instance_id = get_rparam(request, 5);

	service = zbx_deltacloud_get_service(url, key, secret, driver, provider);

	if (service == NULL)
	{
		SET_MSG_RESULT(result, strdup("No Data"));
		return SYSINFO_RET_FAIL;
	}
	
	for (i = 0; service->instances.values_num; i++)
	{
		zbx_deltacloud_instance_t *instance = service->instances.values[i];
		if (0 == strcmp(instance->id, instance_id))
		{
			SET_STR_RESULT(result, strdup(instance->realm_id));
			return SYSINFO_RET_OK;
		}
	}
	SET_MSG_RESULT(result, strdup("Not match data"));
	return SYSINFO_RET_FAIL;
}
int	zbx_module_cloud_instance_realm_href(AGENT_REQUEST *request, AGENT_RESULT *result)
{

	int	i;
	char	*url;
	char	*key;
	char	*secret;
	char	*driver;
	char	*provider;
	char	*instance_id;
	
	zbx_deltacloud_service_t	*service = NULL;
	zbx_deltacloud_instance_t	*deltacloud_instance = NULL;

	if (request->nparam != 6)
	{
		/* set optional error message */
		SET_MSG_RESULT(result, strdup("Invalid number of parameters e.g.) cloud.instane.realm_href[url, key, secret, driver, provider, instance_id]"));
		return SYSINFO_RET_FAIL;
	}
	url = get_rparam(request, 0);
	key = get_rparam(request, 1);
	secret = get_rparam(request, 2);
	driver = get_rparam(request, 3);
	provider = get_rparam(request, 4);
	instance_id = get_rparam(request, 5);

	service = zbx_deltacloud_get_service(url, key, secret, driver, provider);

	if (service == NULL)
	{
		SET_MSG_RESULT(result, strdup("No Data"));
		return SYSINFO_RET_FAIL;
	}
	
	for (i = 0; service->instances.values_num; i++)
	{
		zbx_deltacloud_instance_t *instance = service->instances.values[i];
		if (0 == strcmp(instance->id, instance_id))
		{
			SET_STR_RESULT(result, strdup(instance->realm_href));
			return SYSINFO_RET_OK;
		}
	}
	SET_MSG_RESULT(result, strdup("Not match data"));
	return SYSINFO_RET_FAIL;
}
int	zbx_module_cloud_instance_launch_time(AGENT_REQUEST *request, AGENT_RESULT *result)
{

	int	i;
	char	*url;
	char	*key;
	char	*secret;
	char	*driver;
	char	*provider;
	char	*instance_id;
	
	zbx_deltacloud_service_t	*service = NULL;
	zbx_deltacloud_instance_t	*deltacloud_instance = NULL;

	if (request->nparam != 6)
	{
		/* set optional error message */
		SET_MSG_RESULT(result, strdup("Invalid number of parameters e.g.) cloud.instane.launch_time[url, key, secret, driver, provider, instance_id]"));
		return SYSINFO_RET_FAIL;
	}
	url = get_rparam(request, 0);
	key = get_rparam(request, 1);
	secret = get_rparam(request, 2);
	driver = get_rparam(request, 3);
	provider = get_rparam(request, 4);
	instance_id = get_rparam(request, 5);

	service = zbx_deltacloud_get_service(url, key, secret, driver, provider);

	if (service == NULL)
	{
		SET_MSG_RESULT(result, strdup("No Data"));
		return SYSINFO_RET_FAIL;
	}
	
	for (i = 0; service->instances.values_num; i++)
	{
		zbx_deltacloud_instance_t *instance = service->instances.values[i];
		if (0 == strcmp(instance->id, instance_id))
		{
			SET_STR_RESULT(result, strdup(instance->launch_time));
			return SYSINFO_RET_OK;
		}
	}
	SET_MSG_RESULT(result, strdup("Not match data"));
	return SYSINFO_RET_FAIL;
}
int	zbx_module_cloud_instance_hwp_href(AGENT_REQUEST *request, AGENT_RESULT *result)
{

	int	i;
	char	*url;
	char	*key;
	char	*secret;
	char	*driver;
	char	*provider;
	char	*instance_id;
	
	zbx_deltacloud_service_t	*service = NULL;
	zbx_deltacloud_instance_t	*deltacloud_instance = NULL;

	if (request->nparam != 6)
	{
		/* set optional error message */
		SET_MSG_RESULT(result, strdup("Invalid number of parameters e.g.) cloud.instane.hwp.href[url, key, secret, driver, provider, instance_id]"));
		return SYSINFO_RET_FAIL;
	}
	url = get_rparam(request, 0);
	key = get_rparam(request, 1);
	secret = get_rparam(request, 2);
	driver = get_rparam(request, 3);
	provider = get_rparam(request, 4);
	instance_id = get_rparam(request, 5);

	service = zbx_deltacloud_get_service(url, key, secret, driver, provider);

	if (service == NULL)
	{
		SET_MSG_RESULT(result, strdup("No Data"));
		return SYSINFO_RET_FAIL;
	}
	
	for (i = 0; service->instances.values_num; i++)
	{
		zbx_deltacloud_instance_t *instance = service->instances.values[i];
		if (0 == strcmp(instance->id, instance_id))
		{
			SET_STR_RESULT(result, strdup(instance->hwp->href));
			return SYSINFO_RET_OK;
		}
	}
	SET_MSG_RESULT(result, strdup("Not match data"));
	return SYSINFO_RET_FAIL;
}
int	zbx_module_cloud_instance_hwp_id(AGENT_REQUEST *request, AGENT_RESULT *result)
{

	int	i;
	char	*url;
	char	*key;
	char	*secret;
	char	*driver;
	char	*provider;
	char	*instance_id;
	
	zbx_deltacloud_service_t	*service = NULL;
	zbx_deltacloud_instance_t	*deltacloud_instance = NULL;

	if (request->nparam != 6)
	{
		/* set optional error message */
		SET_MSG_RESULT(result, strdup("Invalid number of parameters e.g.) cloud.instane.hwp.id[url, key, secret, driver, provider, instance_id]"));
		return SYSINFO_RET_FAIL;
	}
	url = get_rparam(request, 0);
	key = get_rparam(request, 1);
	secret = get_rparam(request, 2);
	driver = get_rparam(request, 3);
	provider = get_rparam(request, 4);
	instance_id = get_rparam(request, 5);

	service = zbx_deltacloud_get_service(url, key, secret, driver, provider);

	if (service == NULL)
	{
		SET_MSG_RESULT(result, strdup("No Data"));
		return SYSINFO_RET_FAIL;
	}
	
	for (i = 0; service->instances.values_num; i++)
	{
		zbx_deltacloud_instance_t *instance = service->instances.values[i];
		if (0 == strcmp(instance->id, instance_id))
		{
			SET_STR_RESULT(result, strdup(instance->hwp->id));
			return SYSINFO_RET_OK;
		}
	}
	SET_MSG_RESULT(result, strdup("Not match data"));
	return SYSINFO_RET_FAIL;
}
int	zbx_module_cloud_instance_hwp_name(AGENT_REQUEST *request, AGENT_RESULT *result)
{

	int	i;
	char	*url;
	char	*key;
	char	*secret;
	char	*driver;
	char	*provider;
	char	*instance_id;
	
	zbx_deltacloud_service_t	*service = NULL;
	zbx_deltacloud_instance_t	*deltacloud_instance = NULL;

	if (request->nparam != 6)
	{
		/* set optional error message */
		SET_MSG_RESULT(result, strdup("Invalid number of parameters e.g.) cloud.instane.hwp.name[url, key, secret, driver, provider, instance_id]"));
		return SYSINFO_RET_FAIL;
	}
	url = get_rparam(request, 0);
	key = get_rparam(request, 1);
	secret = get_rparam(request, 2);
	driver = get_rparam(request, 3);
	provider = get_rparam(request, 4);
	instance_id = get_rparam(request, 5);

	service = zbx_deltacloud_get_service(url, key, secret, driver, provider);

	if (service == NULL)
	{
		SET_MSG_RESULT(result, strdup("No Data"));
		return SYSINFO_RET_FAIL;
	}
	
	for (i = 0; service->instances.values_num; i++)
	{
		zbx_deltacloud_instance_t *instance = service->instances.values[i];
		if (0 == strcmp(instance->id, instance_id))
		{
			SET_STR_RESULT(result, strdup(instance->hwp->name));
			return SYSINFO_RET_OK;
		}
	}
	SET_MSG_RESULT(result, strdup("Not match data"));
	return SYSINFO_RET_FAIL;
}
/******************************************************************************
 *                                                                            *
 * Function: zbx_module_init                                                  *
 *                                                                            *
 * Purpose: the function is called on agent startup                           *
 *          It should be used to call any initialization routines             *
 *                                                                            *
 * Return value: ZBX_MODULE_OK - success                                      *
 *               ZBX_MODULE_FAIL - module initialization failed               *
 *                                                                            *
 * Comment: the module won't be loaded in case of ZBX_MODULE_FAIL             *
 *                                                                            *
 ******************************************************************************/
int	zbx_module_init()
{
	/* initialization for dummy.random */
	srand(time(NULL));

	key_t shm_key;
	shm_key = zbx_ftok(CONFIG_FILE, ZBX_IPC_CLOUD_ID);
	zbx_mem_create(&cloud_mem, shm_key, ZBX_NO_MUTEX, MEM_SIZE, "cloud cache size", "CloudCacheSize", 0);

	zabbix_log(LOG_LEVEL_ERR, "-------shm_key : %d---\n", shm_key);
	zabbix_log(LOG_LEVEL_ERR, "-------total_size: %d---\n", cloud_mem->total_size);
	zabbix_log(LOG_LEVEL_ERR, "-------used_size: %d---\n", cloud_mem->used_size);
	deltacloud = __cloud_mem_malloc_func(NULL, sizeof(zbx_deltacloud_t));	
	zabbix_log(LOG_LEVEL_ERR, "-------used_size: %d---\n", cloud_mem->used_size);
	memset(deltacloud, 0, sizeof(zbx_deltacloud_t));
	zabbix_log(LOG_LEVEL_ERR, "-------used_size: %d---\n", cloud_mem->used_size);

	CLOUD_VECTOR_CREATE(&deltacloud->services, ptr);

	return ZBX_MODULE_OK;
}

static void	cloud_address_shared_free(zbx_deltacloud_address_t *address)
{
	if (NULL != address->address)
		__cloud_mem_free_func(address->address);
	__cloud_mem_free_func(address);
	zabbix_log(LOG_LEVEL_ERR, "--free instance-----used_size: %d---\n", cloud_mem->used_size);
}

static void	cloud_hardware_profile_shared_free(zbx_deltacloud_hardware_profile_t *hwp)
{
	if (NULL != hwp->href)
		__cloud_mem_free_func(hwp->href);
	if (NULL != hwp->id)
		__cloud_mem_free_func(hwp->id);
	if (NULL != hwp->name)
		__cloud_mem_free_func(hwp->name);
	__cloud_mem_free_func(hwp);
}


static void	cloud_instance_shared_free(zbx_deltacloud_instance_t *instance)
{
	if (NULL != instance->href)
		__cloud_mem_free_func(instance->href);
	if (NULL != instance->id)
		__cloud_mem_free_func(instance->id);
	if (NULL != instance->name)
		__cloud_mem_free_func(instance->name);
	if (NULL != instance->owner_id)
		__cloud_mem_free_func(instance->owner_id);
	if (NULL != instance->image_id)
		__cloud_mem_free_func(instance->image_id);
	if (NULL != instance->image_href)
		__cloud_mem_free_func(instance->image_href);
	if (NULL != instance->realm_id)
		__cloud_mem_free_func(instance->realm_id);
	if (NULL != instance->realm_href)
		__cloud_mem_free_func(instance->realm_href);
	if (NULL != instance->state)
		__cloud_mem_free_func(instance->state);
	if (NULL != instance->launch_time)
		__cloud_mem_free_func(instance->launch_time);
	__cloud_mem_free_func(instance);
	zbx_vector_ptr_clean(&instance->public_addresses, (zbx_mem_free_func_t)cloud_address_shared_free);
	zbx_vector_ptr_clean(&instance->private_addresses, (zbx_mem_free_func_t)cloud_address_shared_free);
	zbx_vector_ptr_destroy(&instance->public_addresses);
	zbx_vector_ptr_destroy(&instance->private_addresses);
	cloud_hardware_profile_shared_free(instance->hwp);
	zabbix_log(LOG_LEVEL_ERR, "--free instance-----used_size: %d---\n", cloud_mem->used_size);
}

static void	cloud_service_shared_free(zbx_deltacloud_service_t *service)
{
	int i;
	if (NULL != service->url)
		__cloud_mem_free_func(service->url);
	if (NULL != service->key)
		__cloud_mem_free_func(service->key);
	if (NULL != service->secret)
		__cloud_mem_free_func(service->secret);
	if (NULL != service->driver)
		__cloud_mem_free_func(service->driver);
	if (NULL != service->provider)
		__cloud_mem_free_func(service->provider);

	zbx_vector_ptr_clean(&service->instances, (zbx_mem_free_func_t)cloud_instance_shared_free);
	zbx_vector_ptr_destroy(&service->instances);
	__cloud_mem_free_func(service);
	zabbix_log(LOG_LEVEL_ERR, "--free service-----used_size: %d---\n", cloud_mem->used_size);
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_module_uninit                                                *
 *                                                                            *
 * Purpose: the function is called on agent shutdown                          *
 *          It should be used to cleanup used resources if there are any      *
 *                                                                            *
 * Return value: ZBX_MODULE_OK - success                                      *
 *               ZBX_MODULE_FAIL - function failed                            *
 *                                                                            *
 ******************************************************************************/
int	zbx_module_uninit()
{
	int i;

	if (NULL != deltacloud)
	{
		zbx_vector_ptr_clean(&deltacloud->services, (zbx_mem_free_func_t)cloud_service_shared_free);
		zbx_vector_ptr_destroy(&deltacloud->services);
		__cloud_mem_free_func(deltacloud);
		zabbix_log(LOG_LEVEL_ERR, "--free deltacloud-----used_size: %d---\n", cloud_mem->used_size);
	}
	zbx_mem_destroy(cloud_mem);
	zabbix_log(LOG_LEVEL_ERR, "----destroy cloud_mem---used_size: %d---\n", cloud_mem->used_size);

	return ZBX_MODULE_OK;
}
