/***************************************************************************
 * Thomson, Daniel R.     ECE 531 Summer 2024
 *   
 * Thermostat app 
 *	
 * DESCRIPTION: An IoT client that communicates with the cloud to relay data
 *              and respond to programming
 *   
 * OUTPUT: 
 *  [] local    /var/log/syslog
 *  [] local    /tmp/status
 *  [] remote   aws-ec2-server
***************************************************************************/

#include <stdio.h>
#include <syslog.h>     //For syslog()
#include <signal.h>     //For SIGHUP, SIGTERM
#include <stdbool.h>    //For boolean logic
#include <unistd.h>     //For sleep() and fork()
#include <errno.h>      //For errno
#include <string.h>     //For strerror()
#include <sys/stat.h>   //For Umask
#include <sys/types.h>  
#include <stdlib.h>
#include <curl/curl.h>  // for curl 
#include <argp.h>       // for argument parser

#include "thermostat.h"

/**
 * Map an error code to a string.
 *
 * @param err The error code.
 * @return A string related to the error code.
 */
const char* error_to_msg(const int err) 
{
  char* msg = NULL;
  switch(err) {
    case OK:
      msg = "Everything is just fine.";
      break;
    case NO_FORK:
      msg = "Unable to fork a child process.";
      break;
    case NO_SETSID:
      msg = "Unable to set the session id.";
      break;
    case RECV_SIGTERM:
      msg = "Received a termination signal; exiting.";
      break;
    case RECV_SIGKILL:
      msg = "Received a kill signal; exiting.";
      break;
    case REQ_ERR:
      msg = "Requested resources is unavailable.";
      break;
    case NO_FILE:
      msg = "File not found/opened.";
      break;
    case INIT_ERR:
      msg = "Unable to initialze object.";
      break;
    case ERR_CHDIR:
      msg = "Unable to change directories.";
      break;
    case WEIRD_EXIT:
    case ERR_WTF:
      msg = "An unexptected condition has come up, exiting.";
      break;
    case UNKNOWN_HEATER_STATE:
      msg = "Encountered an unknown heater state!";
      break;
    default:
      msg = "You submitted some kind of wackadoodle error code. What's up with you?";
  }
  return msg;
}

static void _exit_process(const int err)
{
  syslog(LOG_INFO, "%s", error_to_msg(err));
  closelog(); 
  exit(err);
}

/*
 * Curl call_back function
 */
static size_t call_back(void *data, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    struct CurlBuffer *buf = (struct CurlBuffer *)userp;

    char *ptr = realloc(buf->response, buf->size + realsize + 1);
    if (ptr == NULL)
    {
        return 0;
    }

    buf->response = ptr;
    memcpy(&(buf->response[buf->size]), data, realsize);
    buf->size += realsize;
    buf->response[buf->size] = 0;

    return realsize;
}

struct CurlBuffer chunk = {0};

/*
 * A helper function to use curl.h to send and ack curl requests
 */
static char * doCurlAction(const char *url, char *message, char *type, bool hasVerb)
{
    CURL *curl = curl_easy_init();
    if (curl)
    {
        FILE* outFile = fopen("curlCache.txt", "wb");
    
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, type);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, outFile);

        if (strcmp(type, "GET") == 0)
        {
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, call_back);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        }

        if (hasVerb)
        {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, message);
        }
        else
        {
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        }

        if (curl_easy_perform(curl) != CURLE_OK)
        {
            return (char *) REQ_ERR;
        }

        curl_easy_cleanup(curl);
    }
    else
    {
        return (char *) INIT_ERR;
    }
    return chunk.response;
}

// argp parser
static error_t parser(int key, char *arg, struct argp_state *state)
{
    struct Arguments *arg_struct = state->input;
    switch (key)
    {
        case 'u':
            arg_struct->url = arg;
            break;
        case 'o':
            arg_struct->post = true;
            break;
        case 'g':
            arg_struct->get = true;
            break;
        case 'p':
            arg_struct->put = true;
            break;
        case 'd':
            arg_struct->delete = true;
            break;
        case ARGP_KEY_NO_ARGS: // Args missing
            if (arg_struct->post == true
                    || arg_struct->put == true
                    || arg_struct->delete == true)
            {
                syslog(LOG_INFO, "Verbs are missing from argument structure.");
                argp_usage(state);
                return REQ_ERR;
            }
            break;
        case ARGP_KEY_ARG: // Too many args
            if (state->arg_num >= 1)
            {
                syslog(LOG_INFO, "Too many arguments, use quotes around your extra argument.");
                argp_usage(state);
                return REQ_ERR;
            }
            arg_struct->arg = arg;
            break;
        case ARGP_KEY_END: // URL malformed
            if (arg_struct->url == NULL)
            {
                syslog(LOG_INFO, "Invalide URL provided.");
                argp_usage(state);
                return REQ_ERR;
            }
            else if (arg_struct->get == false
                    && arg_struct->post == false
                    && arg_struct->put == false
                    && arg_struct->delete == false)
            {
                syslog(LOG_INFO, "http request type missing.");
                argp_usage(state);
                return REQ_ERR;
            }
            break;
        case ARGP_KEY_SUCCESS: // perform request
            if (arg_struct->get) 
            {
                doCurlAction(arg_struct->url, NULL, "GET", false);
                break;
            }
            else if (arg_struct->post)
            {
                 doCurlAction(arg_struct->url, arg_struct->arg, "POST", true);
                 break;
            }
            else if (arg_struct->put)
            {
                doCurlAction(arg_struct->url, arg_struct->arg, "PUT", true);
                break;
            }
            else if (arg_struct->delete)
            {
                doCurlAction(arg_struct->url, arg_struct->arg, "DELETE", true);
                break;
            }
        default:
            return ARGP_ERR_UNKNOWN;
    }
    return OK;
}

/*
 * DISCLAIMER: I had to use argp for this program which is a breaking change from
 * my original implemention and consequently I do owe a great deal of the success
 * of this project to the internet.
 */
static struct argp argp = {opt, parser, usage, description};

/*
 * Read temperature file and publishes the data
 */
static void publishMeasurement(void)
{
    char *buffer = NULL;
    size_t size = 0;

    FILE *fp = fopen(TEMP_PATH, "r");
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    rewind(fp);
    buffer = malloc((size + 1) * sizeof(*buffer)); 
    fread(buffer, size, 1, fp);
    buffer[size] = '\0';
    
    doCurlAction(MEAS_TBL_URL, buffer, "POST", true);
}

/*
 * Code to manage the heater
 */
static int setHeater(char *state)
{
    FILE *fp = fopen(STAT_PATH, "w");
    if (fp == NULL)
    {
        return UNKNOWN_HEATER_STATE;
    }

    fputs(state, fp);
    fclose(fp);
    return OK;
}

/*
 * Handle curl request to know if system should be on or off
 */
static void requestStatus(void)
{
    int code;
    char *status = doCurlAction(STATUS_TBL_URL, NULL, "GET", false);
    char* state = status; //TODO get state from status
    if (strcmp(state, "true") == 0) 
    {
       code = setHeater("ON");
    }
    else 
    {
       code = setHeater("OFF");
    }

    if (code != OK)
    {
        syslog(LOG_INFO, "%s", error_to_msg(code));
    }

    chunk.response = NULL;
    chunk.size = 0;
}

/*
 * Check is a file exists
 */
static bool file_exists(const char* fname)
{
    struct stat buffer;
    return (stat(fname, &buffer) == 0) ? true : false;
}


/*
 * Code taken from example in slides with some small modifications
 */
static void _signal_handler(const int signal)
{
    switch (signal)
    {
        case SIGHUP:
            break;
        case SIGTERM:
            _exit_process(RECV_SIGTERM);
            break;
        default:
            _exit_process(WEIRD_EXIT);
    }
}

static void _handle_fork(const pid_t pid)
{
  // For some reason, we were unable to fork.
  if (pid < 0)
  {
    _exit_process(NO_FORK);
  }

  // Fork was successful so exit the parent process.
  if (pid > 0)
  {
    exit(OK);
  }
}

/*
 * Handler for handfree version on this program
 */
static int runAsDaemon(void)
{
    pid_t pid = fork();

    openlog(DAEMON_NAME, LOG_PID | LOG_NDELAY | LOG_NOWAIT, LOG_DAEMON);

    _handle_fork(pid);

    if (setsid() < -1)
    {
        return NO_SETSID;
    }

    // Closing file descriptors (STDIN, STDOUT, etc.).
    for (long x = sysconf(_SC_OPEN_MAX); x>=0; x--)
    {
        close(x);
    }

    umask(S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

    if (chdir("/") < 0)
    {
       return ERR_CHDIR;
    }

    signal(SIGTERM, _signal_handler);
    signal(SIGHUP, _signal_handler);

    return OK;
}

/*
 * This function is the engine of the program
 */
static int execute(void)
{
    // check operation of "tcsimd"
    if (file_exists(TEMP_PATH) && file_exists(STAT_PATH))
    {
        syslog(LOG_INFO, "Thermocouple succeeded.");
        while (1)
        {
         // read temp and send post to webserver for thermostat
         publishMeasurement();
         // get and respond to the heater status
         requestStatus();
         sleep(3);
        }
        return WEIRD_EXIT;
    }
        syslog(LOG_ERR, "Thermocouple failed.");
        return NO_FILE;
}

/*
 * Program genesis
 */
int main(int argc, char **argv)
{
    int code;
    if (argc > 1)
    {
        syslog(LOG_INFO, "-Using CLI-");
   
        // initialize arguments
        struct Arguments arg_struct;
        arg_struct.url = NULL;
        arg_struct.arg = NULL;
        arg_struct.post = false;
        arg_struct.get = false;
        arg_struct.put = false;
        arg_struct.delete = false;

        // parse arguments
        argp_parse(&argp, argc, argv, 0, 0, &arg_struct);
    }
    else
    {
        syslog(LOG_INFO, "-Using daemon-");
        code = runAsDaemon();
        if (code != OK)
        {
            _exit_process(code);
        }
    }
   
    code = execute();
    if (execute() != OK)
    {
        _exit_process(code);
    }

    return WEIRD_EXIT;
}

