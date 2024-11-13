/*****************************************************************************
 *
 * ALERTA-NEB.C
 *
 *     $ indent -i2 -di1 -br -nut -pcs -l160 -bls alerta-neb.c
 *
 *****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "nagios.h"

#include "uthash.h"

#include <curl/curl.h>
#include <jansson.h>

NEB_API_VERSION (CURRENT_NEB_API_VERSION);

char *NAME = "Nagios-Alerta Gateway";
char *VERSION = "5.0.1";

void *alerta_module_handle = NULL;

int check_handler (int, void *);

int debug = 0;

#define MESSAGE_SIZE        32768
#define VALUE_SIZE          1024
#define LONGDESC_SIZE       MESSAGE_SIZE - 2048
#define HOSTNAME_SIZE       2048
#define URL_SIZE            2048
#define USER_AGENT_SIZE     1024
#define AUTH_HEADER_SIZE    1024
#define ENVIRONMENT_SIZE    1024
#define CUSTOMER_SIZE       1024
#define KEY_SIZE            2048
#define TEMP_SIZE           2048

char message[MESSAGE_SIZE];
char value[VALUE_SIZE];
char long_desc[LONGDESC_SIZE];
char hostname[HOSTNAME_SIZE];
char alert_url[URL_SIZE];
char heartbeat_url[URL_SIZE];
char user_agent[USER_AGENT_SIZE];
char auth_header[AUTH_HEADER_SIZE];
char environment[ENVIRONMENT_SIZE] = "Production";
char customer[CUSTOMER_SIZE];
char temp[TEMP_SIZE];
char hard_states_only = 0;

static CURL *curl = NULL;

typedef struct downtime_struct
{
  char key[KEY_SIZE];
  int id;
  UT_hash_handle hh;
} downtime;

downtime *downtimes = NULL;

const char *
display_evt_type (int type)
{
  switch (type)
  {
  case NEBTYPE_HOSTCHECK_INITIATE:
  case NEBTYPE_HOSTCHECK_PROCESSED:
  case NEBTYPE_HOSTCHECK_RAW_START:
  case NEBTYPE_HOSTCHECK_RAW_END:
  case NEBTYPE_HOSTCHECK_ASYNC_PRECHECK:
  case NEBTYPE_HOSTCHECK_SYNC_PRECHECK:
    return ("HostCheck");
  case NEBTYPE_SERVICECHECK_INITIATE:
  case NEBTYPE_SERVICECHECK_PROCESSED:
  case NEBTYPE_SERVICECHECK_RAW_START:
  case NEBTYPE_SERVICECHECK_RAW_END:
  case NEBTYPE_SERVICECHECK_ASYNC_PRECHECK:
    return ("ServiceCheck");
  default:
    return ("UnknownType");
  }
}

const char *
display_evt_class (int class)
{
  switch (class)
  {
  case NEBTYPE_HOSTCHECK_INITIATE:
  case NEBTYPE_SERVICECHECK_INITIATE:
    return ("Initiate");
  case NEBTYPE_HOSTCHECK_PROCESSED:
  case NEBTYPE_SERVICECHECK_PROCESSED:
    return ("Processed");
  case NEBTYPE_HOSTCHECK_RAW_START:
  case NEBTYPE_SERVICECHECK_RAW_START:
    return ("RawStart");
  case NEBTYPE_HOSTCHECK_RAW_END:
  case NEBTYPE_SERVICECHECK_RAW_END:
    return ("RawEnd");
  case NEBTYPE_HOSTCHECK_ASYNC_PRECHECK:
  case NEBTYPE_SERVICECHECK_ASYNC_PRECHECK:
    return ("AsyncPrecheck");
  case NEBTYPE_HOSTCHECK_SYNC_PRECHECK:
    return ("SyncPrecheck");
  default:
    return ("UnknownType");
  }
}

const char *
display_state (int state)
{
  switch (state)
  {
  case STATE_OK:
    return ("normal");
  case STATE_WARNING:
    return ("warning");
  case STATE_CRITICAL:
    return ("critical");
  case STATE_UNKNOWN:
  default:
    return ("unknown");
  }
}

const char *
display_state_type (int state_type)
{
  switch (state_type)
  {
  case SOFT_STATE:
    return ("Soft");
  case HARD_STATE:
    return ("Hard");
  default:
    return ("Unknown");
  }
}

const char *
display_check_type (int check_type)
{
  switch (check_type)
  {
  //case HOST_CHECK_ACTIVE:
  case SERVICE_CHECK_ACTIVE:
    return ("Active");
  //case HOST_CHECK_PASSIVE:
  case SERVICE_CHECK_PASSIVE:
    return ("Passive");
  default:
    return ("Unknown");
  }
}

const char *
display_downtime_type (int downtime_type)
{
  switch (downtime_type)
  {
  case SERVICE_DOWNTIME:
    return ("Service Downtime");
  case HOST_DOWNTIME:
    return ("Host Downtime");
  default:
    return ("Host or Service Downtime");
  }
}

void
logger (int level, const char *message,...)
{
#ifdef NAEMON
  nm_log (level, "alerta: %s", message);
#else
  logit (level, TRUE, "alerta: %s", message);
#endif
}

void
log_debug (const char *message)
{
  if (debug)
    logger (NSLOG_INFO_MESSAGE, message);
}

void
log_info (const char *message)
{
  logger (NSLOG_INFO_MESSAGE, message);
}

void
log_warning (const char *message)
{
  logger (NSLOG_RUNTIME_WARNING, message);
}

void
log_config (const char *message)
{
  logger (NSLOG_CONFIG_ERROR, message);
}

void
log_error (const char *message)
{
  logger (NSLOG_RUNTIME_ERROR, message);
}

int
log_curl (CURL * handle, curl_infotype type, char *data, size_t size, void *userptr)
{
  snprintf (message, MESSAGE_SIZE, "[curl] %s", data);
  log_debug (message);
}

int
send_to_alerta (char *url, char *message)
{
  CURLcode res;
  long status;

  if (curl == NULL)
  {
    curl = curl_easy_init ();
  } else
  {
    curl_easy_reset (curl);
  }

  if (!curl)
  {
    return NEB_ERROR;
  }
  log_debug (message);

  struct curl_slist *headers = NULL;
  headers = curl_slist_append (headers, "Content-Type: application/json");
  headers = curl_slist_append (headers, "Expect:");     /* disable 100 - continue expectation */

  snprintf (user_agent, USER_AGENT_SIZE, "%s/%s", NAME, VERSION);

  if (strlen (auth_header))
    headers = curl_slist_append (headers, auth_header);
  curl_easy_setopt (curl, CURLOPT_URL, url);
  curl_easy_setopt (curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt (curl, CURLOPT_USERAGENT, user_agent);
  curl_easy_setopt (curl, CURLOPT_POSTFIELDS, message);
  if (debug)
  {
    curl_easy_setopt (curl, CURLOPT_VERBOSE, 1L);
    curl_easy_setopt (curl, CURLOPT_DEBUGFUNCTION, log_curl);
  }
  res = curl_easy_perform (curl);
  curl_slist_free_all (headers);

  if (res != CURLE_OK)
  {
    snprintf (message, MESSAGE_SIZE, "curl_easy_perform() failed: %s", curl_easy_strerror (res));
    log_error (message);
    return res;
  }
  curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &status);

  switch (status)
  {
  case 200:
  case 201:
    snprintf (message, MESSAGE_SIZE, "HTTP response OK (status=%ld)", status);
    log_debug (message);
    break;
  case 202:
    snprintf (message, MESSAGE_SIZE, "HTTP request ignored during blackout period. (status=%ld)", status);
    log_warning (message);
    break;
  case 401:
    snprintf (message, MESSAGE_SIZE, "HTTP auth error. API key not configured? (status=%ld)", status);
    log_config (message);
    break;
  case 403:
    snprintf (message, MESSAGE_SIZE, "HTTP request forbidden or rejected. (status=%ld)", status);
    log_config (message);
    break;
  case 429:
    snprintf (message, MESSAGE_SIZE, "HTTP request rate limited. Too many alerts? (status=%ld)", status);
    log_error (message);
    break;
  default:
    snprintf (message, MESSAGE_SIZE, "HTTP server error (status=%ld)", status);
    log_error (message);
    break;
  }
  return status;
}

int
nebmodule_init (int flags, char *args, nebmodule * handle)
{
  gethostname (hostname, 1023);

  alerta_module_handle = handle;/* save the neb module handle */

  neb_set_module_info (alerta_module_handle, NEBMODULE_MODINFO_TITLE, NAME);
  neb_set_module_info (alerta_module_handle, NEBMODULE_MODINFO_AUTHOR, "Nick Satterly");
  neb_set_module_info (alerta_module_handle, NEBMODULE_MODINFO_COPYRIGHT, "Copyright (c) 2015-2019 Nick Satterly");
  neb_set_module_info (alerta_module_handle, NEBMODULE_MODINFO_VERSION, VERSION);
  neb_set_module_info (alerta_module_handle, NEBMODULE_MODINFO_LICENSE, "MIT License");
  neb_set_module_info (alerta_module_handle, NEBMODULE_MODINFO_DESC, "Nagios Event Broker module that forwards Nagios events to Alerta");

  snprintf (message, MESSAGE_SIZE, "Initialising %s module, v%s", NAME, VERSION);
  log_info (message);

  char endpoint[URL_SIZE] = "";
  char key[KEY_SIZE] = "";
  char *token;
  while ((token = strsep (&args, " ")) != NULL)
  {
    if (strncasecmp (token, "http://", 7) == 0)
      strcpy (endpoint, token);
    if (strncasecmp (token, "https://", 8) == 0)
      strcpy (endpoint, token);
    if (strncasecmp (token, "env=", 4) == 0)
      strcpy (environment, token + 4);
    if (strncasecmp (token, "customer=", 9) == 0)
      strcpy (customer, token + 9);
    if (strncasecmp (token, "key=", 4) == 0)
      strcpy (key, token + 4);
    if (strncasecmp (token, "debug=1", 7) == 0)
      debug = 1;
    if (strncasecmp (token, "hard_only=1", 11) == 0)
      hard_states_only = 1;
  }
  if (strlen (endpoint))
  {
    snprintf (alert_url, URL_SIZE, "%s/alert", endpoint);
    snprintf (heartbeat_url, URL_SIZE, "%s/heartbeat", endpoint);
    if (strlen (key))
      snprintf (auth_header, AUTH_HEADER_SIZE, "Authorization: Key %s", key);
  } else
  {
    log_config ("API endpoint not configured.");
    exit (1);
  }

  if (debug)
    log_info ("debug is on");
  else
    log_info ("debug is off");

  if (hard_states_only)
    log_info ("states=Hard (only)");
  else
    log_info ("states=Hard/Soft");

  curl_global_init (CURL_GLOBAL_ALL);

  neb_register_callback (NEBCALLBACK_HOST_CHECK_DATA, alerta_module_handle, 0, check_handler);
  neb_register_callback (NEBCALLBACK_SERVICE_CHECK_DATA, alerta_module_handle, 0, check_handler);
  neb_register_callback (NEBCALLBACK_DOWNTIME_DATA, alerta_module_handle, 0, check_handler);

  snprintf (message, MESSAGE_SIZE, "Forward service checks, host checks and downtime to %s", endpoint);
  log_info (message);

  return NEB_OK;
}

int
nebmodule_deinit (int flags, int reason)
{
  curl_global_cleanup ();

  neb_deregister_callback (NEBCALLBACK_HOST_CHECK_DATA, check_handler);
  neb_deregister_callback (NEBCALLBACK_SERVICE_CHECK_DATA, check_handler);
  neb_deregister_callback (NEBCALLBACK_DOWNTIME_DATA, check_handler);

  log_info ("NEB callbacks for host and service checks successfully de-registered. Bye.");

  return NEB_OK;
}

int
check_handler (int event_type, void *data)
{
  nebstruct_host_check_data *host_chk_data = NULL;
  nebstruct_service_check_data *svc_chk_data = NULL;
  nebstruct_downtime_data *downtime_data = NULL;

  char cov_environment[KEY_SIZE] = "";
  char cov_customer[KEY_SIZE] = "";
  char cov_service[KEY_SIZE] = "";
  customvariablesmember *customvar = NULL;

  json_t *json;

  switch (event_type)
  {
  case NEBCALLBACK_HOST_CHECK_DATA:

    if ((host_chk_data = (nebstruct_host_check_data *) data))
    {

      if (host_chk_data->type == NEBTYPE_HOSTCHECK_PROCESSED)
      {

        log_debug ("Host check received.");

        host *host_object = host_chk_data->object_ptr;
        customvar = host_object->custom_variables;
        customvariablesmember *cvar;

        for (cvar = customvar; cvar != NULL; cvar = cvar->next)
        {
          if (!strcmp (cvar->variable_name, "ENVIRONMENT"))
          {
            snprintf (cov_environment, KEY_SIZE, "%s", cvar->variable_value);
          }
          if (!strcmp (cvar->variable_name, "CUSTOMER"))
          {
            snprintf (cov_customer, KEY_SIZE, "%s", cvar->variable_value);
          }
          if (!strcmp (cvar->variable_name, "SERVICE"))
          {
            snprintf (cov_service, KEY_SIZE, "%s", cvar->variable_value);
          }
        }

        if (host_chk_data->long_output)
        {
          strncpy (long_desc, host_chk_data->long_output, LONGDESC_SIZE);
        } else if (host_chk_data->output)
        {
          strncpy (long_desc, host_chk_data->output, LONGDESC_SIZE);
        } else
        {
          *long_desc = 0;
        }

#ifdef NAGIOS4
        clear_volatile_macros ();
        grab_host_macros (host_object);
        char *raw_command;
        get_raw_command_line (host_object->check_command_ptr, host_object->check_command, &raw_command, 0);
        char *command_line;
        process_macros (raw_command, &command_line, 0);
#endif
        json = json_object ();
        json_object_set_new (json, "origin", json_pack ("s+", "nagios/", hostname));
        json_object_set_new (json, "resource", json_string (host_chk_data->host_name));
        json_object_set_new (json, "event", json_string ("Host Check"));
        json_object_set_new (json, "group", json_string ("Nagios"));
        json_object_set_new (json, "severity", json_string (display_state (host_chk_data->state)));
        json_object_set_new (json, "environment", json_string (strcmp (cov_environment, "") ? cov_environment : environment));
        json_object_set_new (json, "service", json_pack ("[s]", strcmp (cov_service, "") ? cov_service : "Platform"));
        json_object_set_new (json, "tags", json_pack ("[s+]", "check=", display_check_type (host_chk_data->check_type)));

#ifdef NAGIOS4
        json_object_set_new (json, "attributes", json_pack ("{s:s,s:s}",
                                                            "checkCommand", host_object->check_command,
                                                            "commandLine", command_line
                                                            ));
#endif
        json_object_set_new (json, "text", json_string (long_desc));
        snprintf (value, VALUE_SIZE, "%d/%d (%s)", host_chk_data->current_attempt, host_chk_data->max_attempts, display_state_type (host_chk_data->state_type));
        json_object_set_new (json, "value", json_string (value));
        json_object_set_new (json, "type", json_string ("nagiosHostAlert"));
        json_object_set_new (json, "rawData", json_string (host_chk_data->perf_data ? host_chk_data->perf_data : ""));
        json_object_set_new (json, "customer",
                             strcmp (cov_customer, "") ? json_string (cov_customer) :
                             strcmp (customer, "") ? json_string (customer) : json_null ()
          );

        downtime *dt;
        HASH_FIND_STR (downtimes, host_chk_data->host_name, dt);

        if (dt)
          log_debug ("Host in downtime period -- suppress.");
        else if (hard_states_only && host_chk_data->state_type == SOFT_STATE)
          log_debug ("Host in Soft state -- suppress.");
        else
          			
          // Condition pour envoyer seulement si cov_service est "Infra", "Routeurs" ou "Nas"
          if (strcmp(cov_service, "Infra") != 0 &&
              strcmp(cov_service, "Routeurs") != 0 &&
              strcmp(cov_service, "Nas") != 0) {
            log_debug("Service n'est ni 'Infra', ni 'Routeur', ni 'Toto', suppression de l'envoi à Alerta.");
            break; // Ignorer l'envoi si cov_service ne correspond pas
          }

          send_to_alerta (alert_url, json_dumps (json, 0));

#ifdef NAGIOS4
        free (raw_command);
        free (command_line);
#endif
        json_decref (json);
      }
    }
    break;

  case NEBCALLBACK_SERVICE_CHECK_DATA:

    if ((svc_chk_data = (nebstruct_service_check_data *) data))
    {

      if (svc_chk_data->type == NEBTYPE_SERVICECHECK_PROCESSED)
      {

        if (!strcmp (svc_chk_data->service_description, "Heartbeat"))
        {

          if (svc_chk_data->return_code == STATE_OK)
          {
            log_debug ("Heartbeat service check OK.");

            service *service_object = svc_chk_data->object_ptr;
            customvar = service_object->custom_variables;
            customvariablesmember *cvar;

            for (cvar = customvar; cvar != NULL; cvar = cvar->next)
            {
              if (!strcmp (cvar->variable_name, "CUSTOMER"))
              {
                snprintf (cov_customer, KEY_SIZE, "%s", cvar->variable_value);
              }
            }

            json = json_object ();
            json_object_set_new (json, "origin", json_pack ("s+", "nagios/", svc_chk_data->host_name));
            json_object_set_new (json, "type", json_string ("Heartbeat"));
            json_object_set_new (json, "tags", json_pack ("[s]", VERSION));
            json_object_set_new (json, "customer",
                                 strcmp (cov_customer, "") ? json_string (cov_customer) :
                                 strcmp (customer, "") ? json_string (customer) : json_null ()
              );

            send_to_alerta (heartbeat_url, json_dumps (json, 0));
            json_decref (json);
          } else
          {
            log_warning ("Heartbeat service check failed.");
          }
        } else
        {

          log_debug ("Service check received.");

          service *service_object = svc_chk_data->object_ptr;
          customvar = service_object->custom_variables;
          customvariablesmember *cvar;

          for (cvar = customvar; cvar != NULL; cvar = cvar->next)
          {
            if (!strcmp (cvar->variable_name, "ENVIRONMENT"))
            {
              snprintf (cov_environment, KEY_SIZE, "%s", cvar->variable_value);
            }
            if (!strcmp (cvar->variable_name, "CUSTOMER"))
            {
              snprintf (cov_customer, KEY_SIZE, "%s", cvar->variable_value);
            }
            if (!strcmp (cvar->variable_name, "SERVICE"))
            {
              snprintf (cov_service, KEY_SIZE, "%s", cvar->variable_value);
            }
          }

          if (svc_chk_data->long_output)
          {
            strncpy (long_desc, svc_chk_data->long_output, LONGDESC_SIZE);
          } else if (svc_chk_data->output)
          {
            strncpy (long_desc, svc_chk_data->output, LONGDESC_SIZE);
          } else
          {
            *long_desc = 0;
          }

#ifdef NAGIOS4
          clear_volatile_macros ();
          grab_host_macros (service_object->host_ptr);
          grab_service_macros (service_object);
          char *raw_command;
          get_raw_command_line (service_object->check_command_ptr, service_object->check_command, &raw_command, 0);
          char *command_line;
          process_macros (raw_command, &command_line, 0);
#endif
          json = json_object ();
          json_object_set_new (json, "origin", json_pack ("s+", "nagios/", hostname));
          json_object_set_new (json, "resource", json_string (svc_chk_data->host_name));
          json_object_set_new (json, "event", json_string (svc_chk_data->service_description));
          json_object_set_new (json, "group", json_string ("Nagios"));
          json_object_set_new (json, "severity", json_string (display_state (svc_chk_data->state)));
          json_object_set_new (json, "environment", json_string (strcmp (cov_environment, "") ? cov_environment : environment));
          json_object_set_new (json, "service", json_pack ("[s]", strcmp (cov_service, "") ? cov_service : "Platform"));
          json_object_set_new (json, "tags", json_pack ("[s]", display_check_type (svc_chk_data->check_type)));

#ifdef NAGIOS4
          json_object_set_new (json, "attributes", json_pack ("{s:s,s:s}",
                                                              "checkCommand", service_object->check_command,
                                                              "commandLine", command_line
                                                              ));
#endif
          json_object_set_new (json, "text", json_string (long_desc));
          snprintf (value, VALUE_SIZE, "%d/%d (%s)", svc_chk_data->current_attempt, svc_chk_data->max_attempts, display_state_type (svc_chk_data->state_type));
          json_object_set_new (json, "value", json_string (value));
          json_object_set_new (json, "type", json_string ("nagiosServiceAlert"));
          json_object_set_new (json, "rawData", json_string (svc_chk_data->perf_data ? svc_chk_data->perf_data : ""));
          json_object_set_new (json, "customer",
                               strcmp (cov_customer, "") ? json_string (cov_customer) :
                               strcmp (customer, "") ? json_string (customer) : json_null ()
            );

          downtime *dt;
          char key[KEY_SIZE];
          snprintf (key, KEY_SIZE, "%s~%s", svc_chk_data->host_name, svc_chk_data->service_description);
          HASH_FIND_STR (downtimes, key, dt);

          if (dt)
            log_debug ("Service in downtime period -- suppress.");
          else if (hard_states_only && svc_chk_data->state_type == SOFT_STATE)
            log_debug ("Service in Soft state -- suppress.");
          else
                      // Condition pour envoyer seulement si cov_service est "Infra", "Routeurs" ou "Nas"
          if (strcmp(cov_service, "Infra") != 0 &&
              strcmp(cov_service, "Routeurs") != 0 &&
              strcmp(cov_service, "Nas") != 0) {
            log_debug("Service n'est ni 'Infra', ni 'Routeur', ni 'Toto', suppression de l'envoi à Alerta.");
            break; // Ignorer l'envoi si cov_service ne correspond pas
          }
            send_to_alerta (alert_url, json_dumps (json, 0));

#ifdef NAGIOS4
          free (raw_command);
          free (command_line);
#endif
          json_decref (json);
        }
      }
    }
    break;

  case NEBCALLBACK_DOWNTIME_DATA:

    if ((downtime_data = (nebstruct_downtime_data *) data))
    {

      char key[KEY_SIZE];
      downtime *dt = malloc (sizeof (downtime));
      if (downtime_data->downtime_type == HOST_DOWNTIME)
      {
        snprintf (key, KEY_SIZE, "%s", downtime_data->host_name);
      } else if (downtime_data->downtime_type == SERVICE_DOWNTIME)
      {
        snprintf (key, KEY_SIZE, "%s~%s", downtime_data->host_name, downtime_data->service_description);
      }
      strcpy (dt->key, key);
      dt->id = downtime_data->downtime_id;

      if (downtime_data->type == NEBTYPE_DOWNTIME_START)
      {

        log_debug ("Downtime started.");

        HASH_ADD_STR (downtimes, key, dt);

        json = json_object ();
        json_object_set_new (json, "origin", json_pack ("s+", "nagios/", hostname));
        json_object_set_new (json, "resource", json_string (downtime_data->host_name));
        json_object_set_new (json, "event", json_string (downtime_data->service_description ? downtime_data->service_description : "Host Check"));
        json_object_set_new (json, "group", json_string ("Nagios"));
        json_object_set_new (json, "severity", json_string ("informational"));
        json_object_set_new (json, "environment", json_string (strcmp (cov_environment, "") ? cov_environment : environment));
        json_object_set_new (json, "service", json_pack ("[s]", strcmp (cov_service, "") ? cov_service : "Platform"));
        json_object_set_new (json, "tags", json_pack ("[s+]", "downtime=", display_downtime_type (downtime_data->downtime_type)));
        snprintf (temp, TEMP_SIZE, "DOWNTIME STARTED (%lus) - %s", downtime_data->duration, downtime_data->comment_data);
        json_object_set_new (json, "text", json_string (temp));
        snprintf (temp, TEMP_SIZE, "id=%lu", downtime_data->downtime_id);
        json_object_set_new (json, "value", json_string (temp));
        json_object_set_new (json, "type", json_string (downtime_data->downtime_type == HOST_DOWNTIME ? "nagiosHostAlert" : "nagiosServiceAlert"));
        snprintf (temp, TEMP_SIZE, "%s;%lu;%lu;%d;%lu;%lu;%s;%s", downtime_data->host_name, downtime_data->start_time,
                  downtime_data->end_time, downtime_data->fixed, downtime_data->triggered_by, downtime_data->duration,
                  downtime_data->author_name, downtime_data->comment_data);
        json_object_set_new (json, "rawData", json_string (temp));
        json_object_set_new (json, "customer",
                             strcmp (cov_customer, "") ? json_string (cov_customer) :
                             strcmp (customer, "") ? json_string (customer) : json_null ()
          );

        send_to_alerta (alert_url, json_dumps (json, 0));
        json_decref (json);
      }
      if (downtime_data->type == NEBTYPE_DOWNTIME_STOP)
      {

        log_debug ("Downtime stopped.");

        downtime *dt;
        HASH_FIND_STR (downtimes, key, dt);
        if (dt)
        {
          HASH_DEL (downtimes, dt);
          free (dt);
        }
        json = json_object ();
        json_object_set_new (json, "origin", json_pack ("s+", "nagios/", hostname));
        json_object_set_new (json, "resource", json_string (downtime_data->host_name));
        json_object_set_new (json, "event", json_string (downtime_data->service_description ? downtime_data->service_description : "Host Check"));
        json_object_set_new (json, "group", json_string ("Nagios"));
        json_object_set_new (json, "severity", json_string ("normal"));
        json_object_set_new (json, "environment", json_string (strcmp (cov_environment, "") ? cov_environment : environment));
        json_object_set_new (json, "service", json_pack ("[s]", strcmp (cov_service, "") ? cov_service : "Platform"));
        json_object_set_new (json, "tags", json_pack ("[s+]", "downtime=", display_downtime_type (downtime_data->downtime_type)));
        snprintf (temp, TEMP_SIZE, "DOWNTIME %s - %s",
                  downtime_data->attr == NEBATTR_DOWNTIME_STOP_CANCELLED ? "CANCELLED" : "STOPPED", downtime_data->comment_data);
        json_object_set_new (json, "text", json_string (temp));
        snprintf (temp, TEMP_SIZE, "id=%lu", downtime_data->downtime_id);
        json_object_set_new (json, "value", json_string (temp));
        json_object_set_new (json, "type", json_string (downtime_data->downtime_type == HOST_DOWNTIME ? "nagiosHostAlert" : "nagiosServiceAlert"));
        snprintf (temp, TEMP_SIZE, "%s;%lu;%lu;%d;%lu;%lu;%s;%s", downtime_data->host_name, downtime_data->start_time,
                  downtime_data->end_time, downtime_data->fixed, downtime_data->triggered_by, downtime_data->duration,
                  downtime_data->author_name, downtime_data->comment_data);
        json_object_set_new (json, "rawData", json_string (temp));
        json_object_set_new (json, "customer",
                             strcmp (cov_customer, "") ? json_string (cov_customer) :
                             strcmp (customer, "") ? json_string (customer) : json_null ()
          );

        send_to_alerta (alert_url, json_dumps (json, 0));
        json_decref (json);
      }
    }
    break;

  default:
    log_warning ("ERROR: Callback triggered for unregistered event!");
    break;
  }

  return NEB_OK;
}
