/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2012 Free Software Foundation Europe e.V.
   Copyright (C) 2011-2016 Planets Communications B.V.
   Copyright (C) 2013-2018 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/
/*
 * Kern Sibbald, March MM
 */
/**
 * @file
 * the main program
 */

#include "include/bareos.h"
#include "dird.h"
#include "dird/job.h"
#include "dird/scheduler.h"
#include "dird/socket_server.h"
#include "dird/stats.h"
#include "dird/ua_db.h"
#include "lib/daemon.h"
#include "lib/edit.h"
#include "lib/tls_openssl.h"
#include "lib/bsignal.h"
#include "lib/daemon.h"

#include "cats/cats_backends.h"
#include "cats/sql.h"
#include "cats/sql_pooling.h"
#ifndef HAVE_REGEX_H
#include "lib/bregex.h"
#else
#include <regex.h>
#endif
#ifdef HAVE_DIRENT_H
#include <dirent.h>
#define NAMELEN(dirent) (strlen((dirent)->d_name))
#endif
#ifndef HAVE_READDIR_R
int Readdir_r(DIR *dirp, struct dirent *entry, struct dirent **result);
#endif

/* Forward referenced subroutines */
#if !defined(HAVE_WIN32)
static
#endif
void TerminateDird(int sig);
static bool check_resources();
static bool InitializeSqlPooling(void);
static void cleanup_old_files();
static bool init_sighandler_sighup();

/* Exported subroutines */
extern bool do_reload_config();
extern void InvalidateSchedules();
extern bool ParseDirConfig(ConfigurationParser *config, const char *configfile, int exit_code);
extern void prtmsg(void *sock, const char *fmt, ...);

/* Imported subroutines */
void InitJobServer(int max_workers);
void term_job_server();
void StoreJobtype(LEX *lc, ResourceItem *item, int index, int pass);
void StoreProtocoltype(LEX *lc, ResourceItem *item, int index, int pass);
void StoreLevel(LEX *lc, ResourceItem *item, int index, int pass);
void StoreReplace(LEX *lc, ResourceItem *item, int index, int pass);
void StoreMigtype(LEX *lc, ResourceItem *item, int index, int pass);
void init_device_resources();

static char *runjob = NULL;
static bool background = true;
static bool test_config = false;
static alist *reload_table = NULL;

/* Globals Exported */
DirectorResource *me = NULL;                    /* Our Global resource */
ConfigurationParser *my_config = NULL;             /* Our Global config */
char *configfile = NULL;
void *start_heap;

/* Globals Imported */
extern ResourceItem job_items[];

typedef enum {
   CHECK_CONNECTION,  /* Check catalog connection */
   UPDATE_CATALOG,    /* Ensure that catalog is ok with conf */
   UPDATE_AND_FIX     /* Ensure that catalog is ok, and fix old jobs */
} cat_op;

struct resource_table_reference {
   int JobCount;
   CommonResourceHeader **res_table;
};

static bool CheckCatalog(cat_op mode);

static void FreeSavedResources(resource_table_reference *table)
{
   int num = my_config->r_last_ - my_config->r_first_ + 1;

   if (!table->res_table) {
      return;
   }

   for (int j = 0; j < num; j++) {
      FreeResource(table->res_table[j], my_config->r_first_ + j);
   }
   free(table->res_table);
}

/**
 * Called here at the end of every job that was hooked decrementing the active JobCount.
 * When it goes to zero, no one is using the associated resource table, so free it.
 */
static void ReloadJobEndCb(JobControlRecord *jcr, void *ctx)
{
   int i;
   resource_table_reference *table;

   LockJobs();
   LockRes();

   foreach_alist_index(i, table, reload_table) {
      if (table == (resource_table_reference *)ctx) {
         if (table->JobCount) {
            table->JobCount--;
            if (table->JobCount == 0) {
               Dmsg1(100, "Last reference to old configuration table: %#010x\n", table);
               FreeSavedResources(table);
               reload_table->remove(i);
               free(table);
               break;
            }
         }
      }
   }

   UnlockRes();
   UnlockJobs();
}

/**
 * This allows the message handler to operate on the database by using a pointer
 * to this function. The pointer is needed because the other daemons do not have
 * access to the database. If the pointer is not defined (other daemons), then
 * writing the database is disabled.
 */
static bool DirDbLogInsert(JobControlRecord *jcr, utime_t mtime, char *msg)
{
   int length;
   char ed1[50];
   char dt[MAX_TIME_LENGTH];
   PoolMem query(PM_MESSAGE),
            esc_msg(PM_MESSAGE);

   if (!jcr || !jcr->db || !jcr->db->IsConnected()) {
      return false;
   }
   length = strlen(msg);
   esc_msg.check_size(length * 2 + 1);
   jcr->db->EscapeString(jcr, esc_msg.c_str(), msg, length);

   bstrutime(dt, sizeof(dt), mtime);
   Mmsg(query, "INSERT INTO Log (JobId, Time, LogText) VALUES (%s,'%s','%s')",
        edit_int64(jcr->JobId, ed1), dt, esc_msg.c_str());

   return jcr->db->SqlQuery(query.c_str());
}

static void usage()
{
   fprintf(stderr, _(
PROG_COPYRIGHT
"\nVersion: %s (%s)\n\n"
"Usage: bareos-dir [options]\n"
"        -c <path>   use <path> as configuration file or directory\n"
"        -d <nn>     set debug level to <nn>\n"
"        -dt         don't print timestamp in debug output\n"
"        -f          run in foreground (for debugging)\n"
"        -g <group>  run as group <group>\n"
"        -m          print kaboom output (for debugging)\n"
"        -r <job>    run <job> now\n"
"        -s          no signals (for debugging)\n"
"        -t          test - read configuration and exit\n"
"        -u <user>   run as user <user>\n"
"        -v          verbose user messages\n"
"        -xc         print configuration and exit\n"
"        -xs         print configuration file schema in JSON format and exit\n"
"        -?          print this message.\n"
"\n"), 2000, VERSION, BDATE);

   exit(1);
}


/*********************************************************************
 *
 *         Main BAREOS Director Server program
 *
 */
#if defined(HAVE_WIN32)
#define main BareosMain
#endif

int main (int argc, char *argv[])
{
   int ch;
   JobControlRecord *jcr;
   cat_op mode;
   bool no_signals = false;
   bool export_config = false;
   bool export_config_schema = false;
   char *uid = NULL;
   char *gid = NULL;

   start_heap = sbrk(0);
   setlocale(LC_ALL, "");
   bindtextdomain("bareos", LOCALEDIR);
   textdomain("bareos");

   InitStackDump();
   MyNameIs(argc, argv, "bareos-dir");
   InitMsg(NULL, NULL);              /* initialize message handler */
   daemon_start_time = time(NULL);

   console_command = RunConsoleCommand;

   while ((ch = getopt(argc, argv, "c:d:fg:mr:stu:vx:?")) != -1) {
      switch (ch) {
      case 'c':                    /* specify config file */
         if (configfile != NULL) {
            free(configfile);
         }
         configfile = bstrdup(optarg);
         break;

      case 'd':                    /* set debug level */
         if (*optarg == 't') {
            debug_timestamp = false;
         } else {
            debug_level = atoi(optarg);
            if (debug_level <= 0) {
               debug_level = 1;
            }
         }
         Dmsg1(10, "Debug level = %d\n", debug_level);
         break;

      case 'f':                    /* run in foreground */
         background = false;
         break;

      case 'g':                    /* set group id */
         gid = optarg;
         break;

      case 'm':                    /* print kaboom output */
         prt_kaboom = true;
         break;

      case 'r':                    /* run job */
         if (runjob != NULL) {
            free(runjob);
         }
         if (optarg) {
            runjob = bstrdup(optarg);
         }
         break;

      case 's':                    /* turn off signals */
         no_signals = true;
         break;

      case 't':                    /* test config */
         test_config = true;
         break;

      case 'u':                    /* set uid */
         uid = optarg;
         break;

      case 'v':                    /* verbose */
         verbose++;
         break;

      case 'x':                    /* export configuration/schema and exit */
         if (*optarg == 's') {
            export_config_schema = true;
         } else if (*optarg == 'c') {
            export_config = true;
         } else {
            usage();
         }
         break;

      case '?':
      default:
         usage();
         break;
      }
   }
   argc -= optind;
   argv += optind;

   if (!no_signals) {
      InitSignals(TerminateDird);
   }

   if (argc) {
      if (configfile != NULL) {
         free(configfile);
      }
      configfile = bstrdup(*argv);
      argc--;
      argv++;
   }
   if (argc) {
      usage();
   }

   /*
    * See if we want to drop privs.
    */
   if (geteuid() == 0) {
      drop(uid, gid, false);                    /* reduce privileges if requested */
   }

   if (export_config_schema) {
      PoolMem buffer;

      my_config = new_config_parser();
      InitDirConfig(my_config, configfile, M_ERROR_TERM);
      PrintConfigSchemaJson(buffer);
      printf("%s\n", buffer.c_str());
      goto bail_out;
   }

   my_config = new_config_parser();
   ParseDirConfig(my_config, configfile, M_ERROR_TERM);

   if (export_config) {
      my_config->DumpResources(prtmsg, NULL);
      goto bail_out;
   }

   if (!test_config) {                /* we don't need to do this block in test mode */
      if (background) {
         daemon_start();
         InitStackDump();              /* grab new pid */
      }
   }

   if (InitCrypto() != 0) {
      Jmsg((JobControlRecord *)NULL, M_ERROR_TERM, 0, _("Cryptography library initialization failed.\n"));
      goto bail_out;
   }

   if (!check_resources()) {
      Jmsg((JobControlRecord *)NULL, M_ERROR_TERM, 0, _("Please correct the configuration in %s\n"), my_config->get_base_config_path());
      goto bail_out;
   }

   if (!test_config) {                /* we don't need to do this block in test mode */
      /* Create pid must come after we are a daemon -- so we have our final pid */
      CreatePidFile(me->pid_directory, "bareos-dir",
                      GetFirstPortHostOrder(me->DIRaddrs));
      ReadStateFile(me->working_directory, "bareos-dir",
                      GetFirstPortHostOrder(me->DIRaddrs));
   }

   SetJcrInTsd(INVALID_JCR);
   SetThreadConcurrency(me->MaxConcurrentJobs * 2 +
                          4 /* UA */ + 5 /* sched+watchdog+jobsvr+misc */);
   LmgrInitThread(); /* initialize the lockmanager stack */

#if defined(HAVE_DYNAMIC_CATS_BACKENDS)
   char *backend_dir;

   foreach_alist(backend_dir, me->backend_directories) {
      Dmsg1(100, "backend path: %s\n", backend_dir);
   }

   DbSetBackendDirs(me->backend_directories);
#endif
   LoadDirPlugins(me->plugin_directory, me->plugin_names);

   /*
    * If we are in testing mode, we don't try to fix the catalog
    */
   mode = (test_config) ? CHECK_CONNECTION : UPDATE_AND_FIX;

   if (!CheckCatalog(mode)) {
      Jmsg((JobControlRecord *)NULL, M_ERROR_TERM, 0, _("Please correct the configuration in %s\n"), my_config->get_base_config_path());
      goto bail_out;
   }

   if (test_config) {
      TerminateDird(0);
   }

   if (!InitializeSqlPooling()) {
      Jmsg((JobControlRecord *)NULL, M_ERROR_TERM, 0, _("Please correct the configuration in %s\n"), my_config->get_base_config_path());
      goto bail_out;
   }

   MyNameIs(0, NULL, me->name());    /* set user defined name */

   cleanup_old_files();

   p_db_log_insert = (db_log_insert_func)DirDbLogInsert;

   init_sighandler_sighup();

   InitConsoleMsg(working_directory);

   Dmsg0(200, "Start UA server\n");
   StartSocketServer(me->DIRaddrs);

   StartWatchdog();                  /* start network watchdog thread */

   if (me->jcr_watchdog_time) {
      InitJcrSubsystem(me->jcr_watchdog_time); /* start JobControlRecord watchdogs etc. */
   }

   InitJobServer(me->MaxConcurrentJobs);

   DbgJcrAddHook(DbDebugPrint); /* used to debug BareosDb connexion after fatal signal */

//   init_device_resources();

   StartStatisticsThread();

   Dmsg0(200, "wait for next job\n");
   /* Main loop -- call scheduler to get next job to run */
   while ((jcr = wait_for_next_job(runjob))) {
      RunJob(jcr);                   /* run job */
      FreeJcr(jcr);                  /* release jcr */
      SetJcrInTsd(INVALID_JCR);
      if (runjob) {                   /* command line, run a single job? */
         break;                       /* yes, Terminate */
      }
   }

   TerminateDird(0);

bail_out:
   return 0;
}

/**
 * Cleanup and then exit
 *
 */
#if !defined(HAVE_WIN32)
static
#endif
void TerminateDird(int sig)
{
   static bool already_here = false;

   if (already_here) {                /* avoid recursive temination problems */
      Bmicrosleep(2, 0);              /* yield */
      exit(1);
   }

   already_here = true;
   debug_level = 0;                   /* turn off debug */

   DestroyConfigureUsageString();
   StopStatisticsThread();
   StopWatchdog();
   DbSqlPoolDestroy();
   DbFlushBackends();
   UnloadDirPlugins();
   if (!test_config) {                /* we don't need to do this block in test mode */
      WriteStateFile(me->working_directory, "bareos-dir", GetFirstPortHostOrder(me->DIRaddrs));
      DeletePidFile(me->pid_directory, "bareos-dir", GetFirstPortHostOrder(me->DIRaddrs));
   }
   TermScheduler();
   term_job_server();

   if (runjob) {
      free(runjob);
   }
   if (configfile != NULL) {
      free(configfile);
   }
   if (debug_level > 5) {
      PrintMemoryPoolStats();
   }
   if (my_config) {
      my_config->FreeResources();
      free(my_config);
      my_config = NULL;
   }

   StopSocketServer();
   TermMsg();                        /* Terminate message handler */
   CleanupCrypto();
   CloseMemoryPool();               /* release free memory in pool */
   LmgrCleanupMain();
   sm_dump(false, false);

   exit(sig);
}

/**
 * If we get here, we have received a SIGHUP, which means to reread our configuration file.
 */
#if !defined(HAVE_WIN32)
extern "C"
void SighandlerReloadConfig(int sig, siginfo_t *siginfo, void *ptr)
{
   static bool already_here = false;

   if (already_here) {
      /*
       * Note: don't use Jmsg here, as it could produce a race condition
       * on multiple parallel reloads
       */
      Qmsg(NULL, M_ERROR, 0, _("Already reloading. Request ignored.\n"));
      return;
   }
   already_here = true;
   do_reload_config();
   already_here = false;
}
#endif

static bool init_sighandler_sighup()
{
   bool retval = false;
#if !defined(HAVE_WIN32)
   sigset_t block_mask;
   struct sigaction action;

   /*
    *  while handling SIGHUP signal,
    *  ignore further SIGHUP signals.
    */
   sigemptyset(&block_mask);
   sigaddset(&block_mask, SIGHUP);

   memset(&action, 0, sizeof(action));
   action.sa_sigaction = SighandlerReloadConfig;
   action.sa_mask = block_mask;
   action.sa_flags = SA_SIGINFO;
   sigaction(SIGHUP, &action, NULL);

   retval = true;
#endif
   return retval;
}

bool do_reload_config()
{
   static bool is_reloading = false;
   bool reloaded = false;

   if (is_reloading) {
      /*
       * Note: don't use Jmsg here, as it could produce a race condition
       * on multiple parallel reloads
       */
      Qmsg(NULL, M_ERROR, 0, _("Already reloading. Request ignored.\n"));
      return false;
   }
   is_reloading = true;

   StopStatisticsThread();

   LockJobs();
   LockRes();

   DbSqlPoolFlush();

   resource_table_reference prev_config;
   prev_config.res_table = my_config->save_resources();
   prev_config.JobCount = 0;

   Dmsg0(100, "Reloading config file\n");
   bool ok = ParseDirConfig(my_config, configfile, M_ERROR);

   if (!ok || !check_resources() || !CheckCatalog(UPDATE_CATALOG) || !InitializeSqlPooling()) {

      Jmsg(NULL, M_ERROR, 0, _("Please correct the configuration in %s\n"), my_config->get_base_config_path());
      Jmsg(NULL, M_ERROR, 0, _("Resetting to previous configuration.\n"));

      resource_table_reference temp_config;
      temp_config.res_table = my_config->save_resources();

      int num_rcodes = my_config->r_last_ - my_config->r_first_ + 1;
      for (int i = 0; i < num_rcodes; i++) {
         // restore original config
         my_config->res_head_[i] = prev_config.res_table[i];
      }

      // me is changed above by check_resources()
      me = (DirectorResource *)GetNextRes(R_DIRECTOR, NULL);

      FreeSavedResources(&temp_config);
      goto bail_out;

   } else { // parse config ok

      JobControlRecord *jcr;
      int num_running_jobs = 0;
      resource_table_reference *new_table = NULL;

      InvalidateSchedules();
      foreach_jcr(jcr) {
         if (jcr->getJobType() != JT_SYSTEM) {
            if (!new_table) {
               new_table = (resource_table_reference *)malloc(sizeof(resource_table_reference));
               memcpy(new_table, &prev_config, sizeof(resource_table_reference));
            }
            new_table->JobCount++;
            RegisterJobEndCallback(jcr, ReloadJobEndCb, (void *)new_table);
            num_running_jobs++;
         }
      }
      endeach_jcr(jcr);
      reloaded = true;

      SetWorkingDirectory(me->working_directory);
      Dmsg0(10, "Director's configuration file reread.\n");

      if (num_running_jobs > 0) {
         if (!reload_table) {
            reload_table = New(alist(10, not_owned_by_alist));
         }
         reload_table->push(new_table);
      } else { // no jobs running
         FreeSavedResources(&prev_config);
      }
      StartStatisticsThread();
   }

bail_out:
   UnlockRes();
   UnlockJobs();
   is_reloading = false;
   return reloaded;
}

static int me_psk_server_callback(const char *identity,
                                  unsigned char *psk,
                                  unsigned int max_psk_len) {

   Dmsg0(100, "me_psk_server_callback");
   Dmsg1(100, "Received identity '%s'\n", identity);

   int result = 0;

   if (NULL != me && NULL != me->password.value) {
      if (p_encoding_clear == me->password.encoding) {
         /* plain password */
      } else if (p_encoding_md5 == me->password.encoding) {
         /* md5 string */
         /* convert the PSK key to binary */
         result = (int)hex2bin(me->password.value, psk, max_psk_len);
      } else {
         /* hex password excoding? */
      }
   } else {
      Dmsg0(100, "Passowrd not set in Director Config\n");
   }
   if (result == 0 || result > (int)max_psk_len) {
      Dmsg1(100, "Error, Could not convert PSK key '%s' to binary key\n", me->password.value);
      return 0;
   }

   return result;
}

static int ConsPskServerCallback(const char *identity, unsigned char *psk, unsigned int max_psk_len) {

   Dmsg0(100, "ConsPskServerCallback");
   Dmsg1(100, "Received identity '%s'\n", identity);

   char *psk_key = (char *)"1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A";

   /* convert the PSK key to binary */
   unsigned int ret = hex2bin(psk_key, psk, max_psk_len);
   if (ret <= 0 || ret > max_psk_len) {
      Dmsg1(100, "Could not convert PSK key '%s' to binary key\n", psk_key);
      return 0;
   }

   return ret;
}

/* Authenticate file_deamon connection */
static int client_psk_client_callback(char *identity,
                                     unsigned int max_identity_len,
                                     unsigned char *psk,
                                     unsigned int max_psk_len) {
   Dmsg0(100, "client_psk_server_callback");
   Dmsg1(100, "Received identity '%s'\n", identity);

   char *psk_key = (char *)"1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A";

   /* convert the PSK key to binary */
   unsigned int ret = hex2bin(psk_key, psk, max_psk_len);
   if (ret <= 0 || ret > max_psk_len) {
      Dmsg1(100, "Could not convert PSK key '%s' to binary key\n", psk_key);
      return 0;
   }

   return ret;
}

static int store_psk_client_callback(char *identity,
                                     unsigned int max_identity_len,
                                     unsigned char *psk,
                                     unsigned int max_psk_len) {

   Dmsg0(100, "store_psk_server_callback");
   Dmsg1(100, "Received identity '%s'\n", identity);

   char *psk_key = (char *)"1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A";

   /* convert the PSK key to binary */
   unsigned int ret = hex2bin(psk_key, psk, max_psk_len);
   if (ret <= 0 || ret > max_psk_len) {
      Dmsg1(100, "Could not convert PSK key '%s' to binary key\n", psk_key);
      return 0;
   }

   return ret;
}

/*
 * See if two storage definitions point to the same Storage Daemon.
 *
 * We compare:
 *  - address
 *  - SDport
 *  - password
 */
static inline bool IsSameStorageDaemon(StorageResource *store1, StorageResource *store2)
{
   return store1->SDport == store2->SDport &&
          Bstrcasecmp(store1->address, store2->address) &&
          Bstrcasecmp(store1->password.value, store2->password.value);
}

/**
 * Make a quick check to see that we have all the
 * resources needed.
 *
 *  **** FIXME **** this routine could be a lot more
 *   intelligent and comprehensive.
 */
static bool check_resources()
{
   bool OK = true;
   JobResource *job;
   bool need_tls;
   const char *configfile = my_config->get_base_config_path();

   LockRes();

   job = (JobResource *)GetNextRes(R_JOB, NULL);
   me = (DirectorResource *)GetNextRes(R_DIRECTOR, NULL);
   if (!me) {
      Jmsg(NULL, M_FATAL, 0, _("No Director resource defined in %s\n"
                               "Without that I don't know who I am :-(\n"), configfile);
      OK = false;
      goto bail_out;
   } else {
      /*
       * Sanity check.
       */
      if (me->MaxConsoleConnections > me->MaxConnections) {
         me->MaxConnections = me->MaxConsoleConnections + 10;
      }

      my_config->omit_defaults_ = me->omit_defaults;
      SetWorkingDirectory(me->working_directory);

      /*
       * See if message resource is specified.
       */
      if (!me->messages) {
         me->messages = (MessagesResource *)GetNextRes(R_MSGS, NULL);
         if (!me->messages) {
            Jmsg(NULL, M_FATAL, 0, _("No Messages resource defined in %s\n"), configfile);
            OK = false;
            goto bail_out;
         }
      }

      /*
       * When the user didn't force us we optimize for size.
       */
      if (!me->optimize_for_size && !me->optimize_for_speed) {
         me->optimize_for_size = true;
      } else if (me->optimize_for_size && me->optimize_for_speed) {
         Jmsg(NULL, M_FATAL, 0, _("Cannot optimize for speed and size define only one in %s\n"), configfile);
         OK = false;
         goto bail_out;
      }

      if (GetNextRes(R_DIRECTOR, (CommonResourceHeader *)me) != NULL) {
         Jmsg(NULL, M_FATAL, 0, _("Only one Director resource permitted in %s\n"),
            configfile);
         OK = false;
         goto bail_out;
      }

      /*
       * tls_require implies tls_enable
       */
      if (me->tls_cert.require || me->tls_psk.require) {
         if (have_tls) {
            // me->tls.enable = true;
         } else {
            Jmsg(NULL, M_FATAL, 0, _("TLS required but not compiled in in BAREOS.\n"));
            OK = false;
            goto bail_out;
         }
      }

      need_tls = me->tls_cert.enable || me->tls_cert.authenticate;

      if ((me->tls_cert.certfile == nullptr || me->tls_cert.certfile->empty()) && need_tls) {
         Jmsg(NULL, M_FATAL, 0, _("\"TLS Certificate\" file not defined for Director \"%s\" in %s.\n"), me->name(),configfile);
         OK = false;
         goto bail_out;
      }

      if ((me->tls_cert.keyfile == nullptr || me->tls_cert.keyfile->empty()) && need_tls) {
         Jmsg(NULL,  M_FATAL, 0, _("\"TLS Key\" file not defined for Director \"%s\" in %s.\n"),me->name(), configfile);
         OK = false;
         goto bail_out;
      }

      if (((me->tls_cert.CaCertfile == nullptr || me->tls_cert.CaCertfile->empty()) &&
           (me->tls_cert.CaCertdir == nullptr || me->tls_cert.CaCertdir->empty())) &&
          need_tls && me->tls_cert.VerifyPeer) {
         Jmsg(NULL, M_FATAL, 0, _("Neither \"TLS CA Certificate\" or \"TLS CA"
              " Certificate Dir\" are defined for Director \"%s\" in %s."
              " At least one CA certificate store is required"
              " when using \"TLS Verify Peer\".\n"),
              me->name(), configfile);
         OK = false;
         goto bail_out;
      }
    }

   if (!job) {
      Jmsg(NULL, M_FATAL, 0, _("No Job records defined in %s\n"), configfile);
      OK = false;
      goto bail_out;
   }

   if (!PopulateDefs()) {
      OK = false;
      goto bail_out;
   }

   /*
    * Loop over Jobs
    */
   foreach_res(job, R_JOB) {
      if (job->MaxFullConsolidations && job->JobType != JT_CONSOLIDATE) {
         Jmsg(NULL, M_FATAL, 0, _("MaxFullConsolidations configured in job %s which is not of job type \"consolidate\" in file %s\n"), job->name(), configfile);
         OK = false;
         goto bail_out;
      }

      if (job->JobType != JT_BACKUP &&
          (job->AlwaysIncremental ||
           job->AlwaysIncrementalJobRetention ||
           job->AlwaysIncrementalKeepNumber ||
           job->AlwaysIncrementalMaxFullAge)) {
         Jmsg(NULL, M_FATAL, 0, _("AlwaysIncremental configured in job %s which is not of job type \"backup\" in file %s\n"), job->name(), configfile);
         OK = false;
         goto bail_out;
      }
   }

   /*
    * Loop over Consoles
    */
   ConsoleResource *cons;
   foreach_res(cons, R_CONSOLE) {
      /*
       * tls_require implies tls_enable
       */
      if (cons->tls_cert.require) {
         if (have_tls) {
            // cons->tls_cert.enable = true;
         } else {
            Jmsg(NULL, M_FATAL, 0, _("TLS required but not configured in BAREOS.\n"));
            OK = false;
            goto bail_out;
         }
      }

      need_tls = cons->tls_cert.enable || cons->tls_cert.authenticate;

      if ((cons->tls_cert.certfile == nullptr || cons->tls_cert.certfile->empty()) && need_tls) {
         Jmsg(NULL, M_FATAL, 0, _("\"TLS Certificate\" file not defined for Console \"%s\" in %s.\n"),
            cons->name(), configfile);
         OK = false;
         goto bail_out;
      }

      if ((cons->tls_cert.keyfile == nullptr || cons->tls_cert.keyfile->empty()) && need_tls) {
         Jmsg(NULL, M_FATAL, 0, _("\"TLS Key\" file not defined for Console \"%s\" in %s.\n"),
            cons->name(), configfile);
         OK = false;
         goto bail_out;
      }

      if ((cons->tls_cert.CaCertfile == nullptr || cons->tls_cert.CaCertfile->empty()) &&
          (cons->tls_cert.CaCertdir == nullptr || cons->tls_cert.CaCertdir->empty()) && need_tls &&
          cons->tls_cert.VerifyPeer) {
         Jmsg(NULL, M_FATAL, 0, _("Neither \"TLS CA Certificate\" or \"TLS CA"
            " Certificate Dir\" are defined for Console \"%s\" in %s."
            " At least one CA certificate store is required"
            " when using \"TLS Verify Peer\".\n"),
            cons->name(), configfile);
         OK = false;
         goto bail_out;
      }
   }

   /*
    * Loop over Clients
    */
   me->subscriptions_used = 0;
   ClientResource *client;
   foreach_res(client, R_CLIENT) {
      /*
       * Count the number of clients
       *
       * Only used as indication not an enforced limit.
       */
      me->subscriptions_used++;

      /*
       * tls_require implies tls_enable
       */
      if (client->tls_cert.require) {
         if (have_tls) {
            // client->tls_cert.enable = true;
         } else {
            Jmsg(NULL, M_FATAL, 0, _("TLS required but not configured.\n"));
            OK = false;
            goto bail_out;
         }
      }
      need_tls = client->tls_cert.enable || client->tls_cert.authenticate;
      if ((client->tls_cert.CaCertfile == nullptr || client->tls_cert.CaCertfile->empty()) &&
          (client->tls_cert.CaCertdir == nullptr || client->tls_cert.CaCertdir->empty()) && need_tls) {
         Jmsg(NULL, M_FATAL, 0, _("Neither \"TLS CA Certificate\""
            " or \"TLS CA Certificate Dir\" are defined for File daemon \"%s\" in %s.\n"),
            client->name(), configfile);
         OK = false;
         goto bail_out;
      }
    }

   /*
    * Loop over Storages
    */
   StorageResource *store, *nstore;
   foreach_res(store, R_STORAGE) {
      /*
       * tls_require implies tls_enable
       */
      if (store->tls_cert.require) {
         if (have_tls) {
            // store->tls.enable = true;
         } else {
            Jmsg(NULL, M_FATAL, 0, _("TLS required but not configured.\n"));
            OK = false;
            goto bail_out;
         }
      }

      need_tls = store->tls_cert.enable || store->tls_cert.authenticate;

      if ((store->tls_cert.CaCertfile == nullptr || store->tls_cert.CaCertfile->empty()) &&
          (store->tls_cert.CaCertdir == nullptr || store->tls_cert.CaCertdir->empty()) && need_tls) {
         Jmsg(NULL, M_FATAL, 0, _("Neither \"TLS CA Certificate\""
              " or \"TLS CA Certificate Dir\" are defined for Storage \"%s\" in %s.\n"),
              store->name(), configfile);
         OK = false;
         goto bail_out;
      }

      /*
       * If we collect statistics on this SD make sure any other entry pointing to the same SD does
       * not
       * collect statistics otherwise we collect the same data multiple times.
       */
      if (store->collectstats) {
         nstore = store;
         while ((nstore = (StorageResource *)GetNextRes(R_STORAGE, (CommonResourceHeader *)nstore))) {
            if (IsSameStorageDaemon(store, nstore) && nstore->collectstats) {
               nstore->collectstats = false;
               Dmsg1(200, _("Disabling collectstats for storage \"%s\""
                            " as other storage already collects from this SD.\n"), nstore->name());
            }
         }
      }
   }

   if (OK) {
      CloseMsg(NULL);                    /* close temp message handler */
      InitMsg(NULL, me->messages);       /* open daemon message handler */
      if (me->secure_erase_cmdline) {
         SetSecureEraseCmdline(me->secure_erase_cmdline);
      }
      if (me->log_timestamp_format) {
         SetLogTimestampFormat(me->log_timestamp_format);
      }
   }

bail_out:
   UnlockRes();
   return OK;
}

/**
 * Initialize the sql pooling.
 */
static bool InitializeSqlPooling(void)
{
   bool retval = true;
   CatalogResource *catalog;

   foreach_res(catalog, R_CATALOG) {
      if (!db_sql_pool_initialize(catalog->db_driver,
                                  catalog->db_name,
                                  catalog->db_user,
                                  catalog->db_password.value,
                                  catalog->db_address,
                                  catalog->db_port,
                                  catalog->db_socket,
                                  catalog->disable_batch_insert,
                                  catalog->try_reconnect,
                                  catalog->exit_on_fatal,
                                  catalog->pooling_min_connections,
                                  catalog->pooling_max_connections,
                                  catalog->pooling_increment_connections,
                                  catalog->pooling_idle_timeout,
                                  catalog->pooling_validate_timeout)) {
         Jmsg(NULL, M_FATAL, 0, _("Could not setup sql pooling for Catalog \"%s\", database \"%s\".\n"),
              catalog->name(), catalog->db_name);
         retval = false;
         goto bail_out;
      }
   }

bail_out:
   return retval;
}

/**
 * In this routine,
 *  - we can check the connection (mode=CHECK_CONNECTION)
 *  - we can synchronize the catalog with the configuration (mode=UPDATE_CATALOG)
 *  - we can synchronize, and fix old job records (mode=UPDATE_AND_FIX)
 */
static bool CheckCatalog(cat_op mode)
{
   bool OK = true;

   /* Loop over databases */
   CatalogResource *catalog;
   foreach_res(catalog, R_CATALOG) {
      BareosDb *db;

      /*
       * Make sure we can open catalog, otherwise print a warning
       * message because the server is probably not running.
       */
      db = db_init_database(NULL,
                            catalog->db_driver,
                            catalog->db_name,
                            catalog->db_user,
                            catalog->db_password.value,
                            catalog->db_address,
                            catalog->db_port,
                            catalog->db_socket,
                            catalog->mult_db_connections,
                            catalog->disable_batch_insert,
                            catalog->try_reconnect,
                            catalog->exit_on_fatal);

      if (!db || !db->OpenDatabase(NULL)) {
         Pmsg2(000, _("Could not open Catalog \"%s\", database \"%s\".\n"),
              catalog->name(), catalog->db_name);
         Jmsg(NULL, M_FATAL, 0, _("Could not open Catalog \"%s\", database \"%s\".\n"),
              catalog->name(), catalog->db_name);
         if (db) {
            Jmsg(NULL, M_FATAL, 0, _("%s"), db->strerror());
            Pmsg1(000, "%s", db->strerror());
            db->CloseDatabase(NULL);
         }
         OK = false;
         goto bail_out;
      }

      /* Display a message if the db max_connections is too low */
      if (!db->CheckMaxConnections(NULL, me->MaxConcurrentJobs)) {
         Pmsg1(000, "Warning, settings problem for Catalog=%s\n", catalog->name());
         Pmsg1(000, "%s", db->strerror());
      }

      /* we are in testing mode, so don't touch anything in the catalog */
      if (mode == CHECK_CONNECTION) {
         db->CloseDatabase(NULL);
         continue;
      }

      /* Loop over all pools, defining/updating them in each database */
      PoolResource *pool;
      foreach_res(pool, R_POOL) {
         /*
          * If the Pool has a catalog resource create the pool only
          *   in that catalog.
          */
         if (!pool->catalog || pool->catalog == catalog) {
            CreatePool(NULL, db, pool, POOL_OP_UPDATE);  /* update request */
         }
      }

      /* Once they are created, we can loop over them again, updating
       * references (RecyclePool)
       */
      foreach_res(pool, R_POOL) {
         /*
          * If the Pool has a catalog resource update the pool only
          *   in that catalog.
          */
         if (!pool->catalog || pool->catalog == catalog) {
            UpdatePoolReferences(NULL, db, pool);
         }
      }

      /* Ensure basic client record is in DB */
      ClientResource *client;
      foreach_res(client, R_CLIENT) {
         ClientDbRecord cr;
         /* Create clients only if they use the current catalog */
         if (client->catalog != catalog) {
            Dmsg3(500, "Skip client=%s with cat=%s not catalog=%s\n",
                  client->name(), client->catalog->name(), catalog->name());
            continue;
         }
         Dmsg2(500, "create cat=%s for client=%s\n",
               client->catalog->name(), client->name());
         memset(&cr, 0, sizeof(cr));
         bstrncpy(cr.Name, client->name(), sizeof(cr.Name));
         db->CreateClientRecord(NULL, &cr);
      }

      /* Ensure basic storage record is in DB */
      StorageResource *store;
      foreach_res(store, R_STORAGE) {
         StorageDbRecord sr;
         MediaTypeDbRecord mtr;
         memset(&sr, 0, sizeof(sr));
         memset(&mtr, 0, sizeof(mtr));
         if (store->media_type) {
            bstrncpy(mtr.MediaType, store->media_type, sizeof(mtr.MediaType));
            mtr.ReadOnly = 0;
            db->CreateMediatypeRecord(NULL, &mtr);
         } else {
            mtr.MediaTypeId = 0;
         }
         bstrncpy(sr.Name, store->name(), sizeof(sr.Name));
         sr.AutoChanger = store->autochanger;
         if (!db->CreateStorageRecord(NULL, &sr)) {
            Jmsg(NULL, M_FATAL, 0, _("Could not create storage record for %s\n"),
                 store->name());
            OK = false;
            goto bail_out;
         }
         store->StorageId = sr.StorageId;   /* set storage Id */
         if (!sr.created) {                 /* if not created, update it */
            sr.AutoChanger = store->autochanger;
            if (!db->UpdateStorageRecord(NULL, &sr)) {
               Jmsg(NULL, M_FATAL, 0, _("Could not update storage record for %s\n"),
                    store->name());
               OK = false;
               goto bail_out;
            }
         }
      }

      /* Loop over all counters, defining them in each database */
      /* Set default value in all counters */
      CounterResource *counter;
      foreach_res(counter, R_COUNTER) {
         /* Write to catalog? */
         if (!counter->created && counter->Catalog == catalog) {
            CounterDbRecord cr;
            bstrncpy(cr.Counter, counter->name(), sizeof(cr.Counter));
            cr.MinValue = counter->MinValue;
            cr.MaxValue = counter->MaxValue;
            cr.CurrentValue = counter->MinValue;
            if (counter->WrapCounter) {
               bstrncpy(cr.WrapCounter, counter->WrapCounter->name(), sizeof(cr.WrapCounter));
            } else {
               cr.WrapCounter[0] = 0;  /* empty string */
            }
            if (db->CreateCounterRecord(NULL, &cr)) {
               counter->CurrentValue = cr.CurrentValue;
               counter->created = true;
               Dmsg2(100, "Create counter %s val=%d\n", counter->name(), counter->CurrentValue);
            }
         }
         if (!counter->created) {
            counter->CurrentValue = counter->MinValue;  /* default value */
         }
      }
      /* cleanup old job records */
      if (mode == UPDATE_AND_FIX) {
         db->SqlQuery(BareosDb::SQL_QUERY_cleanup_created_job);
         db->SqlQuery(BareosDb::SQL_QUERY_cleanup_running_job);
      }

      /* Set type in global for debugging */
      SetDbType(db->GetType());

      db->CloseDatabase(NULL);
   }

bail_out:
   return OK;
}

static void cleanup_old_files()
{
   DIR* dp;
   struct dirent *result;
#ifdef USE_READDIR_R
   struct dirent *entry;
#endif
   int rc, name_max;
   int my_name_len = strlen(my_name);
   int len = strlen(me->working_directory);
   POOLMEM *cleanup = GetPoolMemory(PM_MESSAGE);
   POOLMEM *basename = GetPoolMemory(PM_MESSAGE);
   regex_t preg1;
   char prbuf[500];
   berrno be;

   /* Exclude spaces and look for .mail or .restore.xx.bsr files */
   const char *pat1 = "^[^ ]+\\.(restore\\.[^ ]+\\.bsr|mail)$";

   /* Setup working directory prefix */
   PmStrcpy(basename, me->working_directory);
   if (len > 0 && !IsPathSeparator(me->working_directory[len-1])) {
      PmStrcat(basename, "/");
   }

   /* Compile regex expressions */
   rc = regcomp(&preg1, pat1, REG_EXTENDED);
   if (rc != 0) {
      regerror(rc, &preg1, prbuf, sizeof(prbuf));
      Pmsg2(000,  _("Could not compile regex pattern \"%s\" ERR=%s\n"),
           pat1, prbuf);
      goto get_out2;
   }

   name_max = pathconf(".", _PC_NAME_MAX);
   if (name_max < 1024) {
      name_max = 1024;
   }

   if (!(dp = opendir(me->working_directory))) {
      berrno be;
      Pmsg2(000, "Failed to open working dir %s for cleanup: ERR=%s\n",
            me->working_directory, be.bstrerror());
      goto get_out1;
      return;
   }

#ifdef USE_READDIR_R
   entry = (struct dirent *)malloc(sizeof(struct dirent) + name_max + 1000);
   while (1) {
      if ((Readdir_r(dp, entry, &result) != 0) || (result == NULL)) {
#else
   while (1) {
      result = readdir(dp);
      if (result == NULL) {
#endif

         break;
      }
      /* Exclude any name with ., .., not my_name or containing a space */
      if (strcmp(result->d_name, ".") == 0 || strcmp(result->d_name, "..") == 0 ||
          strncmp(result->d_name, my_name, my_name_len) != 0) {
         Dmsg1(500, "Skipped: %s\n", result->d_name);
         continue;
      }

      /* Unlink files that match regexes */
      if (regexec(&preg1, result->d_name, 0, NULL, 0) == 0) {
         PmStrcpy(cleanup, basename);
         PmStrcat(cleanup, result->d_name);
         Dmsg1(100, "Unlink: %s\n", cleanup);
         SecureErase(NULL, cleanup);
      }
   }

#ifdef USE_READDIR_R
   free(entry);
#endif
   closedir(dp);
/* Be careful to free up the correct resources */
get_out1:
   regfree(&preg1);
get_out2:
   FreePoolMemory(cleanup);
   FreePoolMemory(basename);
}
