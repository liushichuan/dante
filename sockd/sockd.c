/*
 * Copyright (c) 1997, 1998, 1999, 2000, 2001, 2002, 2003, 2004, 2005, 2006,
 *               2008, 2009, 2010, 2011, 2012
 *      Inferno Nettverk A/S, Norway.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. The above copyright notice, this list of conditions and the following
 *    disclaimer must appear in all copies of the software, derivative works
 *    or modified versions, and any portions thereof, aswell as in all
 *    supporting documentation.
 * 2. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *      This product includes software developed by
 *      Inferno Nettverk A/S, Norway.
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
 * Inferno Nettverk A/S requests users of this software to return to
 *
 *  Software Distribution Coordinator  or  sdc@inet.no
 *  Inferno Nettverk A/S
 *  Oslo Research Park
 *  Gaustadalléen 21
 *  NO-0349 Oslo
 *  Norway
 *
 * any improvements or extensions that they make and grant Inferno Nettverk A/S
 * the rights to redistribute these changes.
 *
 */

#include "common.h"

static const char rcsid[] =
"$Id: sockd.c,v 1.688 2012/06/02 16:50:37 michaels Exp $";


/*
 * signal handler functions.  Upon reception of signal, "sig" is the real
 * signal value (> 0).  We then set a flag indicating we got a signal,
 * but we don't do anything and return immediately.  Later we are called
 * again, with "sig" having the value -(sig), to indicate we are not
 * executing in the signal handler and it's safe to do whatever we
 * need to do.
 */
static void sigterm(int sig, siginfo_t *sip, void *scp);
static void siginfo(int sig, siginfo_t *sip, void *scp);
static void sigchld(int sig, siginfo_t *sip, void *scp);
static void sigalrm(int sig, siginfo_t *sip, void *scp);
static void sighup(int sig, siginfo_t *sip, void *scp);

#if DEBUG && 0
static void dotest(void);
/*
 * runs some internal tests.
 */
#endif

static void
serverinit(int argc, char *argv[]);
/*
 * Initializes options/sockscf.  "argc" and "argv" should be
 * the arguments passed to main().
 * Exits on failure.
 */

static void
usage(int code);
/*
 * print usage.
 */

static char *
getlimitinfo(void);
/*
 * returns a string with some information about current state and limits.
 */

static void
showversion(void);
/*
 * show version info and exits.
 */

static void
showlicense(void);
/*
 * shows license and exits.
 */

static void
checkconfig(void);
/*
 * Scans through the config, perhaps fixing some things and warning
 * about strange things, or errors out on serious mistakes.
 */

static void
log_clientsend(const int protocol, const sockd_child_t *child,
               const int isresend);
/*
 * Logs some information about sending a client object using protocol
 * "protocol" to child "child".
 */

static void
log_noclientrecv(const char *prefix, const sockd_child_t *child);
/*
 * Logs some information about not trying to receive a new client object
 * from child "child".  "prefix" is prefixed to the logged message.
 */

static void
log_probablytimedout(const struct sockaddr *client, const int childtype);
/*
 * Log that client "client" probably timed out while waiting for a
 * child of type "childtype" to handle it.
 */

static void
handlechildcommand(const unsigned char command, sockd_child_t *child,
                   int *finished);
/*
 * Handles childcommand "command", received from child "child".
 * "finished" is set to true if the child has now finished serving
 * requests and should be removed, or false otherwise.
 */

#if DIAGNOSTIC && HAVE_MALLOC_OPTIONS
extern char *malloc_options;
#endif /* DIAGNOSTIC && HAVE_MALLOC_OPTIONS */

#if HAVE_PROGNAME
extern char *__progname;
#elif SOCKS_SERVER
char *__progname = "sockd";   /* default. */
#elif BAREFOOTD
char *__progname = "barefootd";   /* default. */
#elif COVENANT
char *__progname = "covenantd";   /* default. */
#else
#error "who are we?"
#endif /* HAVE_PROGNAME */

extern char *optarg;

#if !HAVE_SETPROCTITLE
char **argv_cpy;
int argc_cpy;
#endif /* !HAVE_SETPROCTITLE */

#define ELECTRICFENCE   0

#if ELECTRICFENCE
   extern int EF_PROTECT_FREE;
   extern int EF_ALLOW_MALLOC_0;
   extern int EF_ALIGNMENT;
   extern int EF_PROTECT_BELOW;
#endif /* ELECTRICFENCE */


int
main(argc, argv)
   int   argc;
   char   *argv[];
{
   const char *function = "main()";
   const int exitsignalv[] = {
      SIGINT, SIGQUIT, SIGBUS, SIGSEGV, SIGTERM, SIGILL, SIGFPE
#ifdef SIGSYS
      , SIGSYS
#endif /* SIGSYS */
   };
   const size_t exitsignalc = ELEMENTS(exitsignalv);
   const int ignoresignalv[] = {
      SIGPIPE
   };
   const size_t ignoresignalc = ELEMENTS(ignoresignalv);
#ifdef RLIMIT_NPROC
   struct rlimit maxproc;
#endif /* RLIMIT_NPROC */
   struct sockaddr_storage addr;
   socklen_t len;
   struct sigaction sigact;
   struct rlimit rlimit;
   rlim_t minfd;
   fd_set *rset;
   ssize_t p;
   size_t i, dforchild;
   sockd_client_t  saved_clientobject;
   sockd_io_t      saved_ioobject;
   sockd_request_t saved_reqobject;
   int have_saved_clientobject = 0,
       have_saved_reqobject    = 0,
       have_saved_ioobject     = 0;

#if DIAGNOSTIC && HAVE_MALLOC_OPTIONS
   malloc_options = "AFGJP";
#endif /* DIAGNOSTIC && HAVE_MALLOC_OPTIONS */

#if ELECTRICFENCE
   EF_PROTECT_FREE         = 1;
   EF_ALLOW_MALLOC_0       = 1;
   EF_ALIGNMENT            = 0;
   EF_PROTECT_BELOW         = 0;
#endif /* ELECTRICFENCE */

#if !HAVE_SETPROCTITLE
   argc_cpy = argc;
   if ((argv_cpy = malloc(sizeof(*argv_cpy) * (argc + 1))) == NULL)
      serr(EXIT_FAILURE, "%s: %s", function, NOMEM);

   for (i = 0; i < (size_t)argc; i++)
      if ((argv_cpy[i] = strdup(argv[i])) == NULL)
         serr(EXIT_FAILURE, "%s: %s", function, NOMEM);
   argv_cpy[i] = NULL;

   initsetproctitle(argc, argv);

   serverinit(argc_cpy, argv_cpy);
#else
   serverinit(argc, argv);
#endif /* !HAVE_SETPROCTITLE*/

#if DEBUG && 0
   dotest();
   exit(0);
#endif

   showconfig(&sockscf);

   /*
    * close any descriptor we don't need, both in case of chroot(2)
    * and needing every descriptor we can get.
    */

   /* syslog takes one */
   dforchild = sockscf.log.type & LOGTYPE_SYSLOG ? -1 : 0;

   /*
    * assume there is no higher fd-index than 64000 open.  Else if the
    * limit is infinity, this can take an awfully long time on a
    * 64 bit machine.
    */
   for (i = 0, minfd = MIN(64000, getmaxofiles(softlimit));
       (rlim_t)i < minfd;
       ++i)
   {
      size_t j;

      if (descriptorisreserved(i))
         continue;

      ++dforchild; /* descriptor will be usable by child. */

      /* sockets we listen on. */
      for (j = 0; j < sockscf.internalc; ++j) {
         if ((int)i == sockscf.internalv[j].s)
            break;

#if NEED_ACCEPTLOCK
         if (sockscf.option.serverc > 1)
            if ((int)i == sockscf.internalv[j].lock)
               break;
#endif /* NEED_ACCEPTLOCK */
      }

      if (j < sockscf.internalc) /* i is socket we listen on. */
         continue;

      close(i);
   }

   errno = 0;
   newprocinit(); /* in case the above closed a syslog(3) fd. */

   /*
    * Check system limits against what we need.
    * Enough descriptors for each child process? + 2 for the pipes from
    * the child to mother.
    */

   /* CONSTCOND */
   minfd = MAX(SOCKD_NEGOTIATEMAX,
                MAX(SOCKD_REQUESTMAX, SOCKD_IOMAX * FDPASS_MAX)) + 2;

#if BAREFOOTD
   minfd += MIN_UDPCLIENTS;
#endif

   /*
    * need to know max number of open files so we can allocate correctly
    * sized fd_sets.  Also, try to set both it and the max number of
    * processes to the hard limit.
    */

   sockscf.state.maxopenfiles = getmaxofiles(hardlimit);

   slog(LOG_DEBUG, "hard limit for max number of open files is %lu, "
                   "soft limit is %lu",
                   (unsigned long)sockscf.state.maxopenfiles,
                   (unsigned long)getmaxofiles(softlimit));

   if (sockscf.state.maxopenfiles == RLIM_INFINITY)
      sockscf.state.maxopenfiles = getmaxofiles(softlimit);

   if (sockscf.state.maxopenfiles == RLIM_INFINITY) {
      sockscf.state.maxopenfiles = 65536; /* a random big number. */

      slog(LOG_DEBUG, "reducing maxopenfiles from RLIM_INFINITY to %lu",
           (unsigned long)sockscf.state.maxopenfiles);
   }

   if (sockscf.state.maxopenfiles < minfd) {
      slog(LOG_INFO, "have only %lu file descriptors available, but need at "
                     "least %lu according to the configuration.  "
                     "Trying to increase it ...",
                     (unsigned long)sockscf.state.maxopenfiles,
                     (unsigned long)minfd);

      sockscf.state.maxopenfiles = minfd;
   }

   rlimit.rlim_cur = rlimit.rlim_max = sockscf.state.maxopenfiles;

   if (setrlimit(RLIMIT_OFILE, &rlimit) == 0)
      slog(LOG_DEBUG, "max number of file descriptors is now %lu",
                      (unsigned long)sockscf.state.maxopenfiles);
  else
      swarnx("failed to increase the max number of file descriptors  "
             "(setrlimit(RLIMIT_OFILE, {%lu, %lu}): %s.  "
             "Change the kernel/shell's limit, or change the values in %s's "
             "include/config.h.  Otherwise %s will not run reliably",
             (unsigned long)rlimit.rlim_cur, (unsigned long)rlimit.rlim_max,
             strerror(errno),
             PACKAGE,
             PACKAGE);

#ifdef RLIMIT_NPROC
   if (getrlimit(RLIMIT_NPROC, &maxproc) != 0)
      swarn("getrlimit(RLIMIT_NPROC) failed");
   else {
      maxproc.rlim_cur = maxproc.rlim_max;

      if (setrlimit(RLIMIT_NPROC, &maxproc) != 0)
         swarn("setrlimit(RLIMIT_NPROC, { %lu, %lu }) failed",
               (unsigned long)rlimit.rlim_cur,
               (unsigned long)rlimit.rlim_max);
   }
#else /* !RLIMIT_NPROC */
   slog(LOG_DEBUG, "no RLIMIT_NPROC defined on this platform, "
                   "max clients calculation will not be done");
#endif /* !RLIMIT_NPROC */


   /*
    * set up signal handlers.
    */

   bzero(&sigact, sizeof(sigact));
   (void)sigemptyset(&sigact.sa_mask);
   sigact.sa_flags = SA_RESTART | SA_NOCLDSTOP | SA_SIGINFO;

   sigact.sa_sigaction = siginfo;
#if HAVE_SIGNAL_SIGINFO
   if (sigaction(SIGINFO, &sigact, NULL) != 0) {
      swarn("sigaction(SIGINFO)");
      return EXIT_FAILURE;
   }
#endif /* HAVE_SIGNAL_SIGINFO */

   /*
    * same handler, for systems without SIGINFO, as well as systems with 
    * broken ("more secure") signal semantics.
    */
   if (sigaction(SIGUSR1, &sigact, NULL) != 0) {
      swarn("sigaction(SIGUSR1)");
      return EXIT_FAILURE;
   }

   sigact.sa_sigaction = sighup;
   if (sigaction(SIGHUP, &sigact, NULL) != 0) {
      swarn("sigaction(SIGHUP)");
      return EXIT_FAILURE;
   }

   sigact.sa_sigaction = sigchld;
   if (sigaction(SIGCHLD, &sigact, NULL) != 0) {
      swarn("sigaction(SIGCHLD)");
      return EXIT_FAILURE;
   }

   sigact.sa_sigaction = sigterm;
   for (i = 0; (size_t)i < exitsignalc; ++i)
      if (sigaction(exitsignalv[i], &sigact, NULL) != 0)
         swarn("sigaction(%d)", exitsignalv[i]);

   sigact.sa_handler = SIG_IGN;
   for (i = 0; (size_t)i < ignoresignalc; ++i)
      if (sigaction(ignoresignalv[i], &sigact, NULL) != 0)
         swarn("sigaction(%d)", ignoresignalv[i]);

   sigact.sa_flags     = 0;   /* want to be interrupted. */
   sigact.sa_sigaction = sigalrm;
   if (sigaction(SIGALRM, &sigact, NULL) != 0) {
      swarn("sigaction(SIGALRM)");
      return EXIT_FAILURE;
   }

   if (sockscf.option.daemon) {
      if (daemon(1, 0) != 0)
         serr(EXIT_FAILURE, "daemon()");

      close(STDIN_FILENO); /* leave stdout/stderr, but close stdin. */
      *sockscf.state.motherpidv = getpid();   /* we are still main server. */
   }

   newprocinit();

   if (!HAVE_DISABLED_PIDFILE) {
      FILE *fp;

      sockd_priv(SOCKD_PRIV_FILE_WRITE, PRIV_ON);
      if ((fp = fopen(sockscf.option.pidfile, "w")) == NULL) {
         swarn("open(%s)", sockscf.option.pidfile);
         errno = 0;
      }
      sockd_priv(SOCKD_PRIV_FILE_WRITE, PRIV_OFF);

      if (fp != NULL) {
         if (fprintf(fp, "%lu\n", (unsigned long)sockscf.state.pid) == EOF)
            swarn("failed writing pid to pidfile %s", sockscf.option.pidfile);
         else
            sockscf.option.pidfilewritten = 1;

         fclose(fp);
      }
   }

   time(&sockscf.stat.boot);

   /* fork of requested number of servers.  Start at one 'cause we are "it".  */
   for (i = 1; (size_t)i < sockscf.option.serverc; ++i) {
      pid_t pid;

      if ((pid = fork()) == -1)
         swarn("fork()");
      else if (pid == 0) {
         newprocinit();
         sockscf.state.motherpidv[i] = sockscf.state.pid;
         break;
      }
      else
         sockscf.state.motherpidv[i] = pid;
   }

   if (childcheck(CHILD_NEGOTIATE) < SOCKD_FREESLOTS_NEGOTIATE
   ||  childcheck(CHILD_REQUEST)   < SOCKD_FREESLOTS_REQUEST
   ||  childcheck(CHILD_IO)        < SOCKD_FREESLOTS_IO)
      serr(EXIT_FAILURE, "initial childcheck() failed");

#if HAVE_PROFILING && HAVE_MONCONTROL /* XXX is this only needed on Linux? */
moncontrol(1);
#endif /* HAVE_PROFILING && HAVE_MONCONTROL*/

#if PRERELEASE
   slog(LOG_INFO, "\n"
   "   ******************************************************************\n"
#if BAREFOOTD
   "   *** Thank you for testing this %s pre-release.          ***\n"
#elif COVENANT
   "   *** Thank you for testing this %s pre-release.           ***\n"
#elif SOCKS_SERVER
   "   *** Thank you for testing this %s pre-release.              ***\n"
#else
#error "hmm, who are we?"
#endif /* SOCKS_SERVER */
   "   *** Please note pre-releases are always configured in a way    ***\n"
   "   *** that puts a considerably larger load on the running system ***\n"
   "   *** system than the standard releases.                         ***\n"
   "   *** This is to help simulate high-load situations and aid in   ***\n"
   "   *** finding bugs before a full release is done.                ***\n"
   "   ******************************************************************",
   PACKAGE);
#endif /* PRERELEASE */

   if (sockscf.option.debug) {
      slog(LOG_DEBUG, "%s", getlimitinfo());
      sockopts_dump();
   }

   rset = allocate_maxsize_fdset();

   slog(LOG_INFO, "%s/server[%d] v%s running\n",
        PACKAGE, pidismother(sockscf.state.pid), VERSION);

   /*
    * main loop; accept new connections and handle our children.
    * CONSTCOND
    */
   while (1) {
      sockd_child_t *child;
      struct timeval *timeout = NULL, zerotimeout  = { 0, 0 };
      int rbits, havefreeslots, free_negc, free_reqc, free_ioc;


      errno = 0; /* reset for each iteration. */
      rbits = fillset(rset, &free_negc, &free_reqc, &free_ioc);

      if (free_negc  < SOCKD_FREESLOTS_NEGOTIATE
      ||  free_reqc  < SOCKD_FREESLOTS_REQUEST
      ||  free_ioc   < SOCKD_FREESLOTS_IO)
         swarnx("need to add a new child process, but can not due to "
                "resource shortage (%s)",
                strerror(sockscf.child.addchild_errno));

#if BAREFOOTD
      if (!sockscf.state.alludpbounced) {
         slog(LOG_DEBUG,
              "have not bounced all udp sessions yet, setting timeout to 0");
         timeout = &zerotimeout;
      }
#endif /* BAREFOOTD */

      if (have_saved_clientobject
      ||  have_saved_reqobject
      ||  have_saved_ioobject) {
         slog(LOG_DEBUG, "have a saved %s, setting timeout to 0",
              have_saved_clientobject ? "clientobject"
            : have_saved_reqobject    ? "reqobject"
            : have_saved_ioobject     ? "ioobject"
            : "XXX internal error");
         timeout = &zerotimeout;
      }

      slog(LOG_DEBUG, "calling select().  Free negc: %d reqc: %d; ioc: %d",
                      free_negc, free_reqc, free_ioc);

      p = selectn(++rbits,
                  rset,
                  NULL,
                  NULL,
                  NULL,
                  NULL,
                  timeout);

      slog(LOG_DEBUG, "%s: selectn() returned %d (%s)",
                      function, (int)p, strerror(errno));

      if (p == -1) {
         SASSERT(errno == EINTR);
         continue;
      }

      /*
       * Handle our children.
       * First get ack of free slots for requests that did not require
       * sending us any new client objects, as well as eofs from them.
       */
      while ((child = getset(ACKPIPE, rset)) != NULL) {
         unsigned char command;
         int childisbad = 0, childhasfinished = 0;

         errno = 0;
         p = socks_recvfromn(child->ack,
                             &command,
                             sizeof(command),
                             0,
                             0,
                             NULL,
                             NULL,
                             NULL,
                             NULL,
                             NULL);
         clearset(ACKPIPE, child, rset);

         if (p != sizeof(command)) {
            switch (p) {
               case -1:
                  swarn("socks_recvfrom(child->ack) from %schild %lu failed",
                        childtype2string(child->type),
                        (unsigned long)child->pid);
                  break;

               case 0:
                  swarnx("eof from %schild %lu",
                         childtype2string(child->type),
                         (unsigned long)child->pid);
                  break;

               default:
                  swarnx("unexpected byte count from %schild %lu.  "
                         "Expected %lu, got %lu",
                         childtype2string(child->type),
                         (unsigned long)child->pid,
                         (unsigned long)sizeof(command), (unsigned long)p);
            }

            childisbad = 1;
         }
         else
            handlechildcommand(command, child, &childhasfinished);

         if (childhasfinished || childisbad) {
            removechild(child->pid);

           /*
            * Can no longer be sure we have any free slots to handle
            * new clients accept(2)-ed, so restart the loop so we
            * can recalculate.
            */
           continue;
         }
      }

      /*
       * Check if we have any client objects previously received but
       * which we failed to send to a child at that time due to temporary
       * resource shortage.  Not that this is not related to a child not
       * having slots available, but only to a temporal failure, such
       * as the child not having yet drained the socket it receives
       * new objects on.
       */
      if (have_saved_clientobject) {
         sockd_child_t *negchild;

         if ((negchild = nextchild(CHILD_NEGOTIATE, SOCKS_TCP)) == NULL) {
            swarnx("new client from %s dropped: no resources available "
                   "(no free negotiator slots / file descriptors)",
                   &saved_clientobject.from);

            close(saved_clientobject.s);
            continue;
         }

         SASSERTX(negchild != NULL);
         SASSERTX(negchild->freec > 0);

         log_clientsend(SOCKS_TCP, negchild, 1);
         p = send_client(negchild->s, &saved_clientobject, NULL, 0);

         if (p == 0) {
            --negchild->freec;
            ++negchild->sentc;
            ++sockscf.stat.negotiate.sendt;

            close(saved_clientobject.s);
            have_saved_clientobject = 0;
         }
         else {
            if (ERRNOISTMP(errno))
               slog(LOG_INFO, "send_client() of client %s failed again: %s",
                    sockaddr2string(TOSA(&saved_clientobject.from), NULL, 0),
                    strerror(errno));
            else {
               len = sizeof(addr);
               if (getsockname(saved_clientobject.s, TOSA(&addr), &len) != 0) {
                  log_probablytimedout(TOSA(&saved_clientobject.from),
                                       negchild->type);

                  close(saved_clientobject.s);
                  have_saved_clientobject = 0;
               }
            }
         }
      }

      if (have_saved_reqobject && free_reqc > 0) {
         sockd_child_t *reqchild;

         reqchild = nextchild(CHILD_REQUEST, SOCKS_TCP);
         SASSERTX(reqchild != NULL);
         SASSERTX(reqchild->freec > 0);

         log_clientsend(saved_reqobject.state.protocol, reqchild, 1);
         if (send_req(reqchild->s, &saved_reqobject) == 0) {
            --free_reqc;
            --reqchild->freec;
            ++reqchild->sentc;
            ++sockscf.stat.request.sendt;

            close(saved_reqobject.s);
            have_saved_reqobject = 0;
         }
         else {
            if (ERRNOISTMP(errno))
               slog(LOG_INFO, "send_req() of client %s failed again: %s",
                    sockaddr2string(TOSA(&saved_reqobject.from), NULL, 0),
                    strerror(errno));
            else {
               len = sizeof(addr);
               if (getsockname(saved_reqobject.s, TOSA(&addr), &len) != 0)
                  log_probablytimedout(TOSA(&saved_reqobject.from),
                                       reqchild->type);
               else { /* error with target socket presumably. */
#if HAVE_NEGOTIATE_PHASE
                  response_t response;

                  slog(LOG_DEBUG, "send_req() failed: %s", strerror(errno));

                  create_response(NULL,
                                  &saved_reqobject.clientauth,
                                  saved_reqobject.req.version,
                                  errno2reply(errno,
                                              saved_reqobject.req.version),
                                  &response);

                  if (send_response(saved_reqobject.s, &response) != 0) {
                     slog(LOG_DEBUG,
                          "%s: send_response(%d) to %s failed: %s",
                          function,
                          saved_reqobject.s,
                          sockaddr2string(TOSA(&saved_reqobject.from), NULL, 0),
                          strerror(errno));
                  }
#endif /* HAVE_NEGOTIATE_PHASE */
               }

               close(saved_reqobject.s);
               have_saved_reqobject = 0;
            }
         }
      }

      if (have_saved_ioobject && free_ioc > 0) {
         sockd_child_t *iochild;

         iochild = nextchild(CHILD_IO, saved_ioobject.state.protocol);
         SASSERTX(iochild != NULL);
         SASSERTX(iochild->freec > 0);

#if BAREFOOTD
         if (saved_ioobject.state.protocol == SOCKS_UDP)
            SASSERTX(iochild->hasudpsession == 0);
#endif /* BAREFOOTD */

         log_clientsend(saved_ioobject.state.protocol, iochild, 1);
         if (send_io(iochild->s, &saved_ioobject) == 0) {
#if BAREFOOTD
            if (saved_ioobject.state.protocol == SOCKS_UDP) {
               SASSERTX(iochild->hasudpsession == 0);
               ++iochild->hasudpsession;

               slog(LOG_DEBUG, "sent udp session for local address %s "
                               "to io-child %lu",
                               sockaddr2string(TOSA(&saved_ioobject.src.laddr),
                                               NULL,
                                               0),
                               (unsigned long)iochild->pid);
            }
#endif /* BAREFOOTD */

            --free_ioc;
            --iochild->freec;
            ++iochild->sentc;
            ++sockscf.stat.io.sendt;

            close_iodescriptors(&saved_ioobject);
            have_saved_ioobject = 0;
         }
         else {
            if (ERRNOISTMP(errno))
               slog(LOG_INFO,
                    "send_io() of client %s failed again: %s",
                    sockaddr2string(TOSA(&saved_ioobject.control.laddr),
                                    NULL,
                                    0),
                    strerror(errno));
            else {
               len = sizeof(addr);
               if (getsockname(saved_ioobject.control.s, TOSA(&addr), &len)
               != 0) {
                  log_probablytimedout(TOSA(&saved_ioobject.control.laddr),
                                       iochild->type);

                  close_iodescriptors(&saved_ioobject);
                  have_saved_ioobject = 0;
               }
#if HAVE_NEGOTIATE_PHASE
               else { /* error with target socket presumably. */
                  response_t response;

                  slog(LOG_DEBUG, "send_io() failed: %s", strerror(errno));

                  create_response(NULL,
                                  &saved_ioobject.src.auth,
                                  saved_ioobject.state.version,
                                  errno2reply(errno,
                                              saved_ioobject.state.version),
                                  &response);

                  if (send_response(saved_ioobject.control.s, &response) != 0) {
                     slog(LOG_DEBUG,
                          "%s: send_response(%d) to %s failed: %s",
                          function,
                          saved_ioobject.control.s,
                          sockshost2string(&saved_ioobject.control.host,
                                           NULL,
                                           0),
                          strerror(errno));
                  }
               }
#endif /* HAVE_NEGOTIATE_PHASE */
            }
         }
      }

      /*
       * Next, get new client objects from the children.
       * Don't try get more requests than we've calculated we can handle at
       * the start, or we could end up needlessly forking a lot of new
       * processes, while at the same time having a lot of unread
       * SOCKD_FREESLOT messages pending.
       */

      havefreeslots = 1;
      while (havefreeslots && (child = getset(DATAPIPE, rset)) != NULL) {
         unsigned char command = SOCKD_NOP;
         int childhasfinished;

         if (sockd_handledsignals()) {
            /* some child could have been removed from rset */
            havefreeslots = 0;
            break;
         }

         clearset(DATAPIPE, child, rset);
         errno = 0;

         switch (child->type) {
            /*
             * in the order a packet travels between children;
             * negotiate -> request -> io
             * (and in Covenants case, -> io -> negotiate again, sometimes).
             */

            case CHILD_NEGOTIATE: {
               sockd_request_t req;
               sockd_child_t *reqchild;

               if (have_saved_reqobject) {
                  log_noclientrecv("have a previously received reqobject",
                                   child);
                  continue;
               }

               if ((reqchild = nextchild(CHILD_REQUEST, SOCKS_TCP)) == NULL) {
                  slog(LOG_DEBUG, "no request slot available for new client");
                  havefreeslots = 0;
                  break;
               }

               SASSERTX(reqchild->freec > 0);

               slog(LOG_DEBUG, "trying to receive request from %s-child %lu",
                               childtype2string(child->type),
                               (unsigned long)child->pid);

               if ((p = recv_req(child->s, &req)) != 0) {
                  slog(LOG_DEBUG, "recv_req() failed with %ld: %s",
                                  (long)p, strerror(errno));

                  break;
               }
               ++sockscf.stat.negotiate.received;
               command = req.reqinfo.command;

               log_clientsend(req.state.protocol, reqchild, 0);
               if (send_req(reqchild->s, &req) == 0) {
                  --free_reqc;
                  --reqchild->freec;
                  ++reqchild->sentc;
                  ++sockscf.stat.request.sendt;

                  close(req.s);
               }
               else if (ERRNOISTMP(errno)) {
                  saved_reqobject      = req;
                  have_saved_reqobject = 1;
               }
#if HAVE_NEGOTIATE_PHASE
               else {
                  response_t response;

                  slog(LOG_DEBUG, "send_req() failed: %s", strerror(errno));

                  create_response(NULL,
                                  &req.clientauth,
                                  req.req.version,
                                  errno2reply(errno, req.req.version),
                                  &response);

                  if (send_response(req.s, &response) != 0) {
                     slog(LOG_DEBUG,
                          "%s: send_response(%d) to %s failed: %s",
                          function,
                          req.s,
                          sockaddr2string(TOSA(&req.from), NULL, 0),
                          strerror(errno));
                  }

                  close(req.s);
               }
#endif /* HAVE_NEGOTIATE_PHASE */

               break;
            }

            case CHILD_REQUEST: {
               sockd_io_t io;
               sockd_child_t *iochild_tcp, *iochild_udp, *iochild;

               if (have_saved_ioobject) {
                  log_noclientrecv("have a previously received ioobject",
                                   child);
                  continue;
               }

               /*
                * don't know which protocol the request we receive is for
                * until we receive it, so make sure we have space for
                * either possibility
                */

               if ((iochild_tcp = nextchild(CHILD_IO, SOCKS_TCP)) == NULL) {
                  slog(LOG_DEBUG, "no tcp io slot available for new client");
                  havefreeslots = 0;
                  break;
               }

#if BAREFOOTD
               if ((iochild_udp = nextchild(CHILD_IO, SOCKS_UDP)) == NULL) {
                  slog(LOG_DEBUG, "no udp io slot available for new client");
                  havefreeslots = 0;
                  break;
               }

               SASSERTX(iochild_udp->hasudpsession == 0);
#else /* !BAREFOOTD */
               /* any child with a free slot can handle a udp session. */
               iochild_udp = iochild_tcp;
#endif /* !BAREFOOTD */

               slog(LOG_DEBUG, "trying to receive request from %s-child %lu",
                               childtype2string(child->type),
                               (unsigned long)child->pid);

               if ((p = recv_io(child->s, &io)) != 0) {
                  slog(LOG_DEBUG, "recv_io() failed with %ld: %s",
                       (long)p, strerror(errno));

                  break;
               }

               ++sockscf.stat.request.received;
               command = io.reqinfo.command;

               switch (io.state.protocol) {
                  case SOCKS_TCP:
                     iochild = iochild_tcp;
                     break;

                  case SOCKS_UDP:
                     iochild = iochild_udp;
                     break;

                  default:
                     SERRX(io.state.protocol);
               }

               SASSERTX(iochild->freec > 0);

               log_clientsend(io.state.protocol, iochild, 0);
               if (send_io(iochild->s, &io) == 0) {
                  --free_ioc;
                  --iochild->freec;
                  ++iochild->sentc;
                  ++sockscf.stat.io.sendt;

                  close_iodescriptors(&io);
#if BAREFOOTD
                  if (io.state.protocol == SOCKS_UDP) {
                     SASSERTX(iochild->hasudpsession == 0);
                     ++iochild->hasudpsession;

                     slog(LOG_DEBUG, "sent udp session for local address %s "
                                     "to io-child %lu",
                                     sockaddr2string(TOSA(&io.src.laddr),
                                                     NULL,
                                                     0),
                                     (unsigned long)iochild->pid);
                  }
#endif /* BAREFOOTD */
               }
               else if (ERRNOISTMP(errno)) {
                  saved_ioobject      = io;
                  have_saved_ioobject = 1;
               }
#if HAVE_NEGOTIATE_PHASE
               else {
                  response_t response;

                  slog(LOG_DEBUG, "send_io() failed: %s", strerror(errno));

                  create_response(NULL,
                                  &io.src.auth,
                                  io.state.version,
                                  errno2reply(errno, io.state.version),
                                  &response);

                  if (send_response(io.control.s, &response) != 0) {
                     slog(LOG_DEBUG,
                          "%s: send_response(%d) to %s failed: %s",
                          function,
                          io.control.s,
                          sockshost2string(&io.src.host, NULL, 0),
                          strerror(errno));
                  }

                  close_iodescriptors(&io);
               }

#endif /* HAVE_NEGOTIATE_PHASE */

               break;
            }

            case CHILD_IO: {
#if COVENANT
               sockd_client_t client;
               sockd_child_t *negchild;

               if (have_saved_clientobject) {
                  log_noclientrecv("have a previously received clientobject",
                                   child);
                  continue;
               }

               if ((negchild = nextchild(CHILD_NEGOTIATE, SOCKS_TCP)) == NULL) {
                  slog(LOG_DEBUG, "no %s-child available to accept old client",
                       childtype2string(child->type));
                  break;
               }

               SASSERTX(negchild->freec > 0);

               slog(LOG_DEBUG, "trying to receive request from %s-child %lu",
                               childtype2string(child->type),
                               (unsigned long)child->pid);

               if ((p = recv_resentclient(child->s, &client)) != 0) {
                  slog(LOG_DEBUG, "recv_resentclient() failed with %ld: %s",
                       (long)p, strerror(errno));

                  break;
               }

               ++sockscf.stat.io.received;

               command = client.reqinfo.command;

               log_clientsend(SOCKS_TCP, negchild, 0);
               p = send_client(negchild->s, &client, NULL, 0);

               if (p == 0) {
                  --negchild->freec;
                  ++negchild->sentc;
                  ++sockscf.stat.negotiate.sendt;

                  close(client.s);
               }
               else if (ERRNOISTMP(errno)) {
                  saved_clientobject      = client;
                  have_saved_clientobject = 1;
               }
#if HAVE_NEGOTIATE_PHASE
               else {
                  response_t response;

                  slog(LOG_DEBUG, "send_client() failed: %s", strerror(errno));

                  /* XXX missing stuff here. */
                  create_response(NULL,
                                  &client.auth,
                                  client.request.version,
                                  errno2reply(errno, client.request.version),
                                  &response);

                  if (send_response(client.s, &response) != 0) {
                     slog(LOG_DEBUG,
                          "%s: send_response(%d) to %s failed: %s",
                          function,
                          client.s,
                          sockshost2string(&client.request.host, NULL, 0),
                          strerror(errno));
                  }

                  close(client.s);
               }
#endif /* HAVE_NEGOTIATE_PHASE */

#endif /* COVENANT */

               break;
            }

            default:
               SERRX(child->type);
         }

         handlechildcommand(command, child, &childhasfinished);

         if (childhasfinished) {
            removechild(child->pid);
            havefreeslots = 0;
         }
         else if (free_negc == 0
         ||       free_reqc == 0
         ||       free_ioc  == 0) {
            /*
             * Could have created more in the meantime, but better
             * safe than sorry.
             */
            if (sockscf.option.debug >= DEBUG_VERBOSE)
               slog(LOG_DEBUG, "free negc = %d, reqc = %d, ioc = %d",
                               free_negc, free_reqc, free_ioc);

            havefreeslots = 0;
         }
      }

      /*
       * handled our children.  Is there a new connection pending now?
       */
      for (i = 0; i < sockscf.internalc && !have_saved_clientobject; ++i) {
         char accepted[MAXSOCKADDRSTRING];
         sockd_client_t client;

         if (sockd_handledsignals()) {
            havefreeslots = 0;
            break;
         }

#if BAREFOOTD
         if (sockscf.internalv[i].protocol != SOCKS_TCP)
            continue; /* udp handled by io children. */
#endif /* BAREFOOTD */

         /* clear client to silence valgrind */
         bzero(&client, sizeof(client));

         if (FD_ISSET(sockscf.internalv[i].s, rset)) {
            sockd_child_t *negchild;

#if NEED_ACCEPTLOCK
            if (sockscf.option.serverc > 1)
               if (socks_lock(sockscf.internalv[i].lock, 1, 0) != 0)
                  continue;
#endif /* NEED_ACCEPTLOCK */

            /* XXX put this in a while loop, up to SOCKD_FREESLOTS? */
            len       = sizeof(client.from);
            client.s  = acceptn(sockscf.internalv[i].s,
                                TOSA(&client.from),
                                &len);
            client.to = sockscf.internalv[i].addr;

#if NEED_ACCEPTLOCK
            if (sockscf.option.serverc > 1)
               socks_unlock(sockscf.internalv[i].lock);
#endif /* NEED_ACCEPTLOCK */

            if (client.s  == -1) {
               switch (errno) {
#ifdef EPROTO
                  case EPROTO:         /* overloaded SVR4 error */
#endif /* EPROTO */
                  case EWOULDBLOCK:    /* BSD   */
                  case ENOBUFS:        /* HPUX  */
                  case ECONNABORTED:   /* POSIX */

                  /* rest appears to be Linux stuff according to Apache src. */
#ifdef ECONNRESET
                  case ECONNRESET:
#endif /* ECONNRESET */
#ifdef ETIMEDOUT
                  case ETIMEDOUT:
#endif /* ETIMEDOUT */
#ifdef EHOSTUNREACH
                  case EHOSTUNREACH:
#endif /* EHOSTUNREACH */
#ifdef ENETUNREACH
                  case ENETUNREACH:
#endif /* ENETUNREACH */

                     if ((sockscf.option.serverc > 1 && ERRNOISTMP(errno))
                     ||  errno == ECONNABORTED)
                        slog(LOG_DEBUG, "accept(2) failed: %s",
                             strerror(errno));
                     else
                        swarn("accept(2) failed");

                     /*
                      * assume connection was aborted/failed/was taken by
                      * another (if serverc > 1) process.
                      */
                     continue;

                  case ENFILE:
                  case EMFILE:
                     swarn("could not accept new client");
                     continue;

                  default:
                     SERR(client.s);
               }
            }

            gettimeofday(&client.accepted, NULL);
            ++sockscf.stat.accepted;

#if HAVE_LINUX_BUGS
            /*
             * yes, Linux manages to lose the descriptor flags. :-(
             * Workaround might be insufficient.
             */
            if (fcntl(client.s, F_SETFL,
                      fcntl(sockscf.internalv[i].s, F_GETFL, 0)) != 0)
               swarn("attempt to work around Linux bug via fcntl() failed");
#endif /* HAVE_LINUX_BUGS */

            slog(LOG_DEBUG,
                 "accepted tcp client %s on address %s, socket %d",
                sockaddr2string(TOSA(&client.from), accepted, sizeof(accepted)),
                 sockaddr2string(TOSA(&sockscf.internalv[i].addr), NULL, 0),
                 sockscf.internalv[i].s);

            if ((negchild = nextchild(CHILD_NEGOTIATE, SOCKS_TCP)) == NULL) {
               swarnx("new client from %s dropped: no resources available "
                      "(no free negotiator slots / file descriptors)",
                      accepted);

               close(client.s);
               continue;
            }

            log_clientsend(SOCKS_TCP, negchild, 0);
            p = send_client(negchild->s, &client, NULL, 0);

            if (p == 0) {
               --free_negc;
               --negchild->freec;
               ++negchild->sentc;
               ++sockscf.stat.negotiate.sendt;

               close(client.s);
            }
            else {
               if (ERRNOISTMP(errno)) {
                  slog(LOG_INFO, "send_client() of client %s failed: %s",
                       sockaddr2string(TOSA(&client.from), NULL, 0),
                       strerror(errno));

                  saved_clientobject      = client;
                  have_saved_clientobject = 1;
               }
               else {
                  slog(LOG_DEBUG, "send_client() failed: %s", strerror(errno));
                  close(client.s);
               }
            }
         }
      }
   }

   /* NOTREACHED */
}

static void
usage(code)
   int code;
{

   fprintf(code == 0 ? stdout : stderr,
   "%s v%s.  Copyright (c) 1997 - 2010, Inferno Nettverk A/S, Norway.\n"
   "usage: %s [-DLNVdfhnv]\n"
   "   -D             : run in daemon mode\n"
   "   -L             : shows the license for this program\n"
   "   -N <number>    : fork of <number> servers [1]\n"
   "   -V             : verify configuration and exit\n"
   "   -d <number>    : set degree of debugging\n"
   "   -f <filename>  : use <filename> as configuration file [%s]\n"
   "   -h             : print this information\n"
   "   -n             : disable TCP keep-alive\n"
   "   -p <filename>  : write pid to <filename> [%s]\n"
   "   -v             : print version info\n",
   PACKAGE, VERSION,
   __progname,
   SOCKD_CONFIGFILE,
   SOCKD_PIDFILE);

   exit(code);
}

static void
showversion(void)
{

   if (strlen(DANTE_BUILD) > 0)
      printf("%s: %s v%s (%s)\n", __progname, PACKAGE, VERSION, DANTE_BUILD);
   else
      printf("%s: %s v%s\n", __progname, PACKAGE, VERSION);
   exit(EXIT_SUCCESS);
}

static void
showlicense(void)
{

   printf("%s: %s v%s\n%s\n", __progname, PACKAGE, VERSION,
"\
/*\n\
 * Copyright (c) 1997, 1998, 1999, 2000, 2001, 2002, 2003, 2004, 2005, 2006,\n\
 *               2007, 2008, 2009, 2010, 2011, 2012\n\
 *      Inferno Nettverk A/S, Norway.  All rights reserved.\n\
 *\n\
 * Redistribution and use in source and binary forms, with or without\n\
 * modification, are permitted provided that the following conditions\n\
 * are met:\n\
 * 1. The above copyright notice, this list of conditions and the following\n\
 *    disclaimer must appear in all copies of the software, derivative works\n\
 *    or modified versions, and any portions thereof, aswell as in all\n\
 *    supporting documentation.\n\
 * 2. All advertising materials mentioning features or use of this software\n\
 *    must display the following acknowledgement:\n\
 *      This product includes software developed by\n\
 *      Inferno Nettverk A/S, Norway.\n\
 * 3. The name of the author may not be used to endorse or promote products\n\
 *    derived from this software without specific prior written permission.\n\
 *\n\
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR\n\
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES\n\
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. \n\
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,\n\
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT\n\
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,\n\
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY\n\
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT \n\
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF\n\
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.\n\
 *\n\
 * Inferno Nettverk A/S requests users of this software to return to\n\
 * \n\
 *  Software Distribution Coordinator  or  sdc@inet.no\n\
 *  Inferno Nettverk A/S\n\
 *  Oslo Research Park\n\
 *  Gaustadalléen 21\n\
 *  NO-0349 Oslo\n\
 *  Norway\n\
 * \n\
 * any improvements or extensions that they make and grant Inferno Nettverk A/S\n\
 * the rights to redistribute these changes.\n\
 *\n\
 */");

   exit(EXIT_SUCCESS);
}

static void
serverinit(argc, argv)
   int argc;
   char *argv[];
{
   const char *function = "serverinit()";
   int ch, verifyonly = 0;

#if !HAVE_PROGNAME
   if (argv[0] != NULL) {
      if ((__progname = strrchr(argv[0], '/')) == NULL)
         __progname = argv[0];
      else
         ++__progname;
   }
#endif /* !HAVE_PROGNAME */


   sockscf.child.addchild = 1;
   sockscf.state.euid     = geteuid();
   sockscf.state.type     = CHILD_MOTHER;
   sockscf.option.serverc = 1;   /* ourselves. ;-) */

   sockscf.shmemfd        = -1;
   sockscf.hostfd         = -1;
   sockscf.loglock        = -1;

   sockscf.option.hosts_access = 0;
   sockscf.option.debugrunopt = -1;

   while ((ch = getopt(argc, argv, "DLN:Vd:f:hlnp:v")) != -1) {
      switch (ch) {
         case 'D':
            sockscf.option.daemon = 1;
            break;

         case 'L':
            showlicense();
            /* NOTREACHED */

         case 'N': {
            char *endptr;

            if ((sockscf.option.serverc = (int)strtol(optarg, &endptr, 10)) < 1
            ||  *endptr != NUL)
               serrx(EXIT_FAILURE, "%s: illegal value for -%c: %s",
               function, ch, optarg);

            break;
         }

         case 'V':
            verifyonly = 1;
            break;

         case 'd':
            sockscf.option.debugrunopt = atoi(optarg);
            sockscf.option.debug       = sockscf.option.debugrunopt;
            break;

         case 'f':
            sockscf.option.configfile = optarg;
            break;

         case 'h':
            usage(0);
            /* NOTREACHED */

         case 'l':
            swarnx("option -%c is deprecated", ch);
            break;

         case 'n':
            sockscf.option.keepalive = 0;
            break;

         case 'p':
            sockscf.option.pidfile = optarg;
            break;

         case 'v':
            showversion();
            /* NOTREACHED */

         default:
            usage(1);
      }
   }

   argc -= optind;
   argv += optind;

   if ((sockscf.state.motherpidv = malloc(sizeof(*sockscf.state.motherpidv)
                                          * sockscf.option.serverc)) == NULL)
      serrx(EXIT_FAILURE, "%s", NOMEM);

   bzero(sockscf.state.motherpidv, sizeof(*sockscf.state.motherpidv)
                                   * sockscf.option.serverc);

   /* we are the main server. */
   *sockscf.state.motherpidv = sockscf.state.pid = getpid();

   if (argc > 0)
      serrx(EXIT_FAILURE, "%s: unknown argument %s", function, *argv);

   if (sockscf.option.configfile == NULL)
      sockscf.option.configfile = SOCKD_CONFIGFILE;

   if (sockscf.option.pidfile == NULL)
      sockscf.option.pidfile = SOCKD_PIDFILE;

   /*
    * needs to be before config file read, as config file may access
    * hostcache.
    */
   hostcachesetup();

#if HAVE_LDAP
   ldapcachesetup();
#endif /* HAVE_LDAP */

   optioninit();
   genericinit();

   /*
    * Pre-config/initial settings that we want to save.
    * Needs to be done after genericinit() so the logging is done to
    * the correct place.
    */

#if HAVE_SCHED_SETSCHEDULER
   if ((sockscf.initial.cpu.policy = sched_getscheduler(0)) == -1)
      swarn("%s: failed to retrieve current schedulingpolicy", function);
   else {
      errno = 0;
      if ((sockscf.initial.cpu.param.sched_priority
      = getpriority(PRIO_PROCESS, 0)) == -1 && errno != 0)
         serr(EXIT_FAILURE,
              "%s: failed to retrieve current scheduling priority", function);
      else {
         char *policy;

         sockscf.initial.cpu.scheduling_isset = 1;

         if ((policy = numeric2cpupolicy(sockscf.initial.cpu.policy)) == NULL) {
            swarnx("%s: unknown initial cpu scheduling policy, value %d",
                   function, sockscf.initial.cpu.policy);
            policy = "<unknown>";
         }

         slog(LOG_DEBUG, "%s: current scheduling policy is %s/%d",
                         function,
                         policy,
                         sockscf.initial.cpu.param.sched_priority);
      }
   }
#endif /* HAVE_SCHED_SETSCHEDULER */

#if HAVE_SCHED_SETAFFINITY
   if (cpu_getaffinity(0,
                       sizeof(sockscf.initial.cpu.mask),
                       &sockscf.initial.cpu.mask) == -1)
      serr(EXIT_FAILURE, "%s: could not get current scheduling affinity",
           function);

   sockscf.initial.cpu.affinity_isset = 1;
#endif /* HAVE_SCHED_SETAFFINITY */

   sockscf.state.cpu = sockscf.initial.cpu;

   checkconfig();

   if (verifyonly) {
      showconfig(&sockscf);
      exit(EXIT_SUCCESS);
   }

   newprocinit();

   if (sockd_initprivs() != 0) {
      swarn("%s: could not initialize privileges, so will run without.  %s",
            function,
            geteuid() == 0 ?
               "" : "Usually we need to be started by root if special "
                    "privileges are to be enabled");

      errno = 0;
   }

   shmem_setup();

   if (bindinternal(SOCKS_TCP) != 0)
      serr(EXIT_FAILURE, "%s: failed to bind internal addresses()", function);

   sockscf.state.inited = 1;
}

/* ARGSUSED */
static void
sigterm(sig, sip, scp)
   int sig;
   siginfo_t *sip;
   void *scp;
{
   const char *function = "sigterm()";

   if (sig > 0) {
      if (SIGNALISOK(sig)) {
         /*
          * A safe signal, but we don't know where we are at this
          * point, and our logging uses some non-signal safe functions,
          * so don't risk exiting and logging now.
          * Instead the code in the normal flow will check for gotten
          * signals and call us if set.
          */

         sockd_pushsignal(sig, sip);
         return;
      }
      else {
         /*
          * A bad signal, something has crashed.  Can't count
          * on it being possible to continue from here, have
          * to exit now.
          */

         sockscf.state.insignal = sig;
         swarnx("%s: terminating on unexpected signal %d", function, sig);

         /*
          * Reinstall default signal handler for this signal and raise it
          * again, assuming we will terminate and get a coredump if that is
          * the default behavior.
          */
         if (signal(sig, SIG_DFL) == SIG_ERR)
            serr(EXIT_FAILURE,
                 "%s: failed to reinstall original handler for signal %d",
                 function, sig);

#if HAVE_LIVEDEBUG
         socks_flushrb();
#endif /* HAVE_LIVEDEBUG */

         raise(sig);

         return; /* need to exit this signal handler so the default can run. */
      }
   }
   else
      sig = -sig;

   slog(LOG_INFO, "%s: exiting on signal %d", function, sig);
   sockdexit(EXIT_SUCCESS);
}

/* ARGSUSED */
static void
siginfo(sig, sip, scp)
   int sig;
   siginfo_t *sip;
   void *scp;
{
   const char *function = "siginfo()";
   const int errno_s = errno, debug_s = sockscf.option.debug;
   unsigned long seconds, days, hours, minutes,
                  free_negc, free_reqc, free_ioc,
                  max_negc, max_reqc, max_ioc;
   size_t clients;

   if (sig > 0) {
      sockd_pushsignal(sig, sip);
      return;
   }

   sig = -sig;

   slog(LOG_DEBUG, "%s", function);

   clients = 0;
   clients += (max_negc = childcheck(-CHILD_NEGOTIATE));
   clients += (max_reqc = childcheck(-CHILD_REQUEST));
   clients += (max_ioc  = childcheck(-CHILD_IO));

   clients -= (free_negc = childcheck(CHILD_NEGOTIATE));
   clients -= (free_reqc = childcheck(CHILD_REQUEST));
   clients -= (free_ioc  = childcheck(CHILD_IO));

   seconds = ROUNDFLOAT(difftime(time(NULL), sockscf.stat.boot));
   seconds2days(&seconds, &days, &hours, &minutes);

   sockscf.option.debug = 1;
   slog(LOG_DEBUG, "%s v%s up %lu day%s, %lu:%.2lu, a: %lu, h: %lu c: %lu",
         PACKAGE,
         VERSION,
         days,
         days == 1 ? "" : "s",
         hours,
         minutes,
         (unsigned long)sockscf.stat.accepted,
         (unsigned long)sockscf.stat.negotiate.sendt,
         (unsigned long)clients);

   slog(LOG_DEBUG, "negotiators (%lu): a: %lu, h: %lu, c: %lu, f: %lu",
        max_negc / SOCKD_NEGOTIATEMAX,
        (unsigned long)sockscf.stat.negotiate.sendt,
        (unsigned long)sockscf.stat.negotiate.received,
        max_negc - free_negc,
        free_negc);

   slog(LOG_DEBUG, "requesters (%lu): a: %lu, h: %lu, c: %lu, f: %lu",
        max_reqc / SOCKD_REQUESTMAX,
        (unsigned long)sockscf.stat.request.sendt,
        (unsigned long)sockscf.stat.request.received,
        max_reqc - free_reqc,
        free_reqc);

   slog(LOG_DEBUG, "iorelayers (%lu): a: %lu, h: %lu, c: %lu, f: %lu",
        max_ioc / SOCKD_IOMAX,
        (unsigned long)sockscf.stat.io.sendt,
        (unsigned long)sockscf.stat.io.received,
        max_ioc - free_ioc,
        free_ioc);

   slog(LOG_DEBUG, "%s", getlimitinfo());

   sockscf.option.debug = debug_s;

   /*
    * Regarding kill(2), the OpenBSD manpage says this:
    *
    * """
    * Setuid and setgid processes are dealt with slightly differently.
    * For the non-root user, to prevent attacks against such processes,
    * some signal deliveries are not permitted and return the error
    * EPERM.  The following signals are allowed through to this class
    * of processes: SIGKILL, SIGINT, SIGTERM, SIGSTOP, SIGTTIN, SIGTTOU,
    * SIGTSTP, SIGHUP, SIGUSR1, SIGUSR2.
    * """
    *
    * The practical effect of this seems to be that if we use different
    * userids, we, when running with the euid of something other than root,
    * may not be able to send the SIGINFO signal to our child. :-/
    * Simlar problem exists for FreeBSD.  
    * 
    * To workaround the problem, send SIGUSR1 to the children instead of 
    * SIGINFO, as SIGUSR1 has always been treated the same way as SIGINFO 
    * by Dante due to some platforms not having the SIGINFO signal.
    */

#ifdef SIGINFO
   if (sig == SIGINFO)
      sig = SIGUSR1;
#endif

   if (pidismother(sockscf.state.pid) == 1)   /* main mother */
      sigserverbroadcast(sig);

   sigchildbroadcast(sig, CHILD_NEGOTIATE | CHILD_REQUEST | CHILD_IO);

   errno = errno_s;
}

/* ARGSUSED */
static void
sighup(sig, sip, scp)
   int sig;
   siginfo_t *sip;
   void *scp;
{
   const char *function = "sighup()";
   const int errno_s = errno;
   listenaddress_t *oldinternalv;
   size_t oldinternalc, i;

   if (sig > 0) {
      sockd_pushsignal(sig, sip);
      return;
   }

   sig = -sig;

#if 0 /* Doesn't work as many systems don't bother to fill in si_pid. :-/. */
   if (!pidismother(sockscf.state.pid) == 1 /* we are not mother. */
   &&  !pidismother(sip->si_pid)) /* and the signal is not from mother. */ {
      swarnx("%s: received SIGHUP from process id %lu, but only expecting it "
             "from mother, so ignoring the signal",
             function, (unsigned long)sip->si_pid);

      return;
   }
#endif

   if (pidismother(sockscf.state.pid) != 1) {
      /*
       * we are not main mother.  Can we assume the signal is from mother?
       * If not, ignore it.
       */
      static int lastsighupid;

      if (lastsighupid == sockscf.shmeminfo->sighupid) {
         /*
          * mothers sighupid has not changed, meaning she has not gotten any
          * sighup.
          */
         swarnx("%s: received SIGHUP, but it does not seem to have been sent "
                "by mother.   Ignoring it.",
                function);

          return;
      }

      lastsighupid = sockscf.shmeminfo->sighupid;
   }

   slog(LOG_INFO, "%s: got SIGHUP, reloading ...", function);

   /*
    * Copy the current addresses on the internal interfaces so that after
    * we have read in the new configuration, we can compare the old list
    * against the new to know which addresses/sockets are longer in use,
    * and stop listening on them.
    *
    * We can not simply clear them before reading in the new config
    * and then start listening on them (again) after we in read the new
    * config, as that would mean we could lose clients in the time-gap
    * between unbinding and rebinding the addresses.
    *
    * This is mainly for barefootd, where adding/removing bounce-to
    * addresses is probably not uncommon.  In the case of barefootd,
    * we additionally have udp addresses we listen on constantly that
    * we need to handle in a similar way.
    *
    * We also have a slight problem with udp rules, as we need to
    * know if the rule existed before the reload.  If it did,
    * we will fail when we try to bind on the internal side,
    * and also waste time trying to set up bouncing for the same
    * udp addresses several times.  More importantly, we will not 
    * know whether the error is expected, or if we should tell the
    * user he is trying to use an address already in use by
    * somebody else.
    * The same problem occurs if we have multiple rules with the
    * same "to:" address, which can make sense provided "from:"
    * differs.  We then have multiple acls for the same "to:" address,
    * but of course only one "to:" address/socket.
    *
    * Our solution for this is to also save the unique udp addresses we
    * need to listen to, and compare against them upon config reload.
    * If one of the udp address is the same as before, we consider the
    * session to be "bounced" already, and if one of the addresses
    * present on the old list is not present on the new list, we know
    * we have an old session/socket to terminate.
    */

    oldinternalc      = sockscf.internalc;
    if ((oldinternalv = malloc(sizeof(*oldinternalv) * oldinternalc))
    == NULL) {
      swarn("%s: failed to allocate memory for saving state before "
            "configuration reload",
            function);

      return;
   }

   for (i = 0; i < oldinternalc; ++i)
      oldinternalv[i] = sockscf.internalv[i];

   genericinit();
   checkconfig();

   /* delay this as long as possible. */
   if (pidismother(sockscf.state.pid) == 1)
      ++sockscf.shmeminfo->sighupid;

   shmem_setup();

   for (i = 0; i < oldinternalc; ++i) {
      ssize_t p;

      p = addrindex_on_listenlist(sockscf.internalc,
                                  sockscf.internalv,
                                  TOSA(&oldinternalv[i].addr),
                                  oldinternalv[i].protocol);

      if (p >= 0) {
         /*
          * this socket/session should continue to exist.
          */
         sockscf.internalv[p].s = oldinternalv[i].s;
         continue;
      }

      /*
       * this socket should be removed.
       */

      if (oldinternalv[i].protocol == SOCKS_TCP) {
         if (pidismother(sockscf.state.pid) == 1) {  /* main mother. */
            close(oldinternalv[i].s);
#if NEED_ACCEPTLOCK
         close(oldinternalv[i].lock);
#endif /* NEED_ACCEPTLOCK */
         }

         continue;
      }

#if BAREFOOTD
      /* else; udp. */

      if (pidismother(sockscf.state.pid) == 1) { /* main mother. */
         slog(LOG_DEBUG, "%s: child should remove udp session for %s",
                         function,
                         sockaddr2string(TOSA(&oldinternalv[i].addr), NULL, 0));
      }
      else {
         switch (sockscf.state.type) {
            case CHILD_IO:
               io_remove_session(TOSA(&oldinternalv[i].addr),
                                 oldinternalv[i].protocol,
                                 IO_ADMINTERMINATION);
               break;

         }
      }
#endif /* BAREFOOTD */
   }

   if (pidismother(sockscf.state.pid) == 1)
      if (bindinternal(SOCKS_TCP) != 0)
         serr(EXIT_FAILURE, "%s: failed to bind internal addresses()",
              function);

   if (pidismother(sockscf.state.pid) == 1) { /* main mother. */
#if BAREFOOTD
      if (!sockscf.state.alludpbounced) {
         /*
          * Go through all rules and see if the current udp addresses
          * to bind matches any of the old ones so we know which addresses
          * are new and need to be bounced.  Those already bounced we should
          * ignore.
          */
         rule_t *rule;

         /*
          * Assume there are no new addresses to bounce initially.
          */
         sockscf.state.alludpbounced = 1;

         for (rule = sockscf.crule; rule != NULL; rule = rule->next) {
            sockshost_t hosttobind;
            struct sockaddr_storage addrtobind;

            if (!rule->state.protocol.udp)
               continue;

            switch (rule->dst.atype) {
               case SOCKS_ADDR_IPV4:
                  ruleaddr2sockshost(&rule->dst, &hosttobind, SOCKS_UDP);
                  sockshost2sockaddr(&hosttobind, TOSA(&addrtobind));

                  if (addrindex_on_listenlist(oldinternalc, oldinternalv,
                  TOSA(&addrtobind), SOCKS_UDP) != -1) {
                     slog(LOG_DEBUG,
                          "%s: marking address %s in rule %lu as bounced; "
                          "previously bounced",
                          function,
                          sockaddr2string(TOSA(&addrtobind), NULL, 0),
                          (unsigned long)rule->number);

                     rule->bounced = 1;
                  }
                  break;

               case SOCKS_ADDR_DOMAIN: {
                  size_t i;

                  i = 0;
                  while (hostname2sockaddr(rule->dst.addr.domain, i++,
                  TOSA(&addrtobind)) != NULL) {
                     if (addrindex_on_listenlist(oldinternalc, oldinternalv,
                     TOSA(&addrtobind), SOCKS_UDP) != -1) {
                        slog(LOG_DEBUG,
                             "%s: marking address %s in rule %lu "
                             "as bounced; previously bounced",
                             function,
                             sockaddr2string(TOSA(&addrtobind), NULL, 0),
                             (unsigned long)rule->number);

                        rule->bounced = 1;
                        break;
                     }
                  }
                  break;
               }

               case SOCKS_ADDR_IFNAME: {
                  size_t i;

                  i = 0;
                  while (ifname2sockaddr(rule->dst.addr.ifname, i++,
                  TOSA(&addrtobind), NULL) != NULL) {
                     if (addrindex_on_listenlist(oldinternalc, oldinternalv,
                     TOSA(&addrtobind), SOCKS_UDP) != -1) {
                        slog(LOG_DEBUG,
                             "%s: marking address %s in rule %lu "
                             "as bounced; previously bounced",
                             function,
                             sockaddr2string(TOSA(&addrtobind), NULL, 0),
                            (unsigned long)rule->number);

                        rule->bounced = 1;
                        break;
                     }
                  }
                  break;
               }

               default:
                  SERRX(rule->dst.atype);
            }

            if (!rule->bounced)
               sockscf.state.alludpbounced = 0;
         }
      }

      /* may have added addresses in new config, rebind if necessary. */
      if (bindinternal(SOCKS_TCP) != 0)
         serr(EXIT_FAILURE, "%s: failed to bind internal addresses()",
         function);

#endif /* BAREFOOTD */

      showconfig(&sockscf);
      sigserverbroadcast(sig);
   }

   free(oldinternalv);

   switch (sockscf.state.type) {
      case CHILD_MOTHER:
         sockd_setcpusettings(&sockscf.cpu.mother);
         break;

      case CHILD_NEGOTIATE:
         sockd_setcpusettings(&sockscf.cpu.negotiate);
         break;

      case CHILD_REQUEST:
         sockd_setcpusettings(&sockscf.cpu.request);
         break;

      case CHILD_IO:
         sockd_setcpusettings(&sockscf.cpu.io);
         break;

      default:
         SERRX(sockscf.state.type);
   }

   if (pidismother(sockscf.state.pid)) /* a mother. */
      sigchildbroadcast(sig, CHILD_NEGOTIATE | CHILD_REQUEST | CHILD_IO);
   else {
      switch (sockscf.state.type) {
         case CHILD_IO:
            io_handlesighup();
            break;
      }
   }

   slog(LOG_INFO, "%s: finished SIGHUP reloading ...", function);

   time(&sockscf.stat.configload);
   errno = errno_s;
}

/* ARGSUSED */
static void
sigchld(sig, sip, scp)
   int sig;
   siginfo_t *sip;
   void *scp;
{
   const char *function = "sigchld()";
   static int deaths;
   pid_t pid;
   int status;

   if (sig > 0) {
      sockd_pushsignal(sig, sip);
      return;
   }

   slog(LOG_DEBUG, "%s", function);

   while (1) {
      pid = waitpid(WAIT_ANY, &status, WNOHANG);

      if (pid == -1 && errno == EINTR)
         continue;

      if (pid <= 0)
         break;

      slog(LOG_DEBUG, "%s: process %lu exited", function, (unsigned long)pid);

      if (pidismother(pid))
         sockscf.state.motherpidv[pidismother(pid) - 1] = 0;

      /*
       * else;  assume relay child.
       * The reason we have to check if the child is known is that we also
       * call removechild() if the child appears to have become "bad",
       * or signals us that it has exited via eof.  I.e., by the time we
       * get here, the child could already have been removed.
       */

      if (getchild(pid) != CHILD_NOTOURS)
         removechild(pid);

      ++deaths;
   }

   if (sockscf.child.maxidle.negotiate == 0
   &&  sockscf.child.maxidle.request   == 0
   &&  sockscf.child.maxidle.io        == 0) {
      /*
       * If maxidle is not set, and many children suddenly die, that
       * probably means something is wrong, so check for that.
       */
      static time_t deathtime;

      if (deathtime == 0)
         time(&deathtime);

      if (difftime(time(NULL), deathtime) > 10) { /* enough time; reset.  */
         deaths = 0;
         time(&deathtime);
      }

      if (deaths >= 10) {
         if (deaths == 10) { /* only log once. */
            slog(LOG_ERR, "%s: %d child deaths in %.0fs.  "
                           "Locking count for a while",
                           function, deaths, difftime(time(NULL), deathtime));

            sockscf.child.addchild       = 0;
            sockscf.child.addchild_errno = 0;
         }

         time(&deathtime); /* once the ball starts rolling... */
         alarm(10);
      }
      else {
         sockscf.child.addchild       = 1; /* if we could not before; can now */
         sockscf.child.addchild_errno = 0;
      }
   }

   sockscf.child.addchild = 1; /* if we could not before, now we can. */
}

/* ARGSUSED */
static void
sigalrm(sig, sip, scp)
   int sig;
   siginfo_t *sip;
   void *scp;
{

   sockscf.child.addchild = 1;
}

static void
checkconfig(void)
{
   const char *function = "checkconfig()";
#if HAVE_PAM
   char *pamservicename = NULL;
#endif /* HAVE_PAM */
#if HAVE_BSDAUTH
   char *bsdauthstylename = NULL;
#endif /* HAVE_BSDAUTH */
#if HAVE_GSSAPI
   char *gssapiservicename = NULL, *gssapikeytab = NULL;
#endif /* HAVE_GSSAPI */
/* XXX same for LDAP */
   uid_t euid;
   rule_t *basev[]       =  { sockscf.crule,         sockscf.srule     };
   int isclientrulev[]   =  { 1,                     0                 };
#if HAVE_PAM || HAVE_BSDAUTH || HAVE_GSSAPI
   int *methodbasev[]    =  { sockscf.clientmethodv, sockscf.methodv   };
   size_t *methodbasec[] =  { &sockscf.clientmethodc, &sockscf.methodc };
#endif /* HAVE_PAM || HAVE_BSDAUTH || HAVE_GSSAPI */
   size_t i, basec;

#if !HAVE_DUMPCONF
#if !HAVE_PRIVILEGES
   if (!sockscf.uid.privileged_isset) {
      sockscf.uid.privileged       = sockscf.state.euid;
      sockscf.uid.privileged_isset = 1;
   }
   else {
      if (socks_seteuid(&euid, sockscf.uid.privileged) != 0
      ||  socks_seteuid(NULL, euid)                    != 0)
         serr(EXIT_FAILURE, "%s: socks_seteuid() failed", function);
   }

   if (!sockscf.uid.unprivileged_isset) {
      sockscf.uid.unprivileged       = sockscf.state.euid;
      sockscf.uid.unprivileged_isset = 1;
   }
   else { /* check the euid-switching works. */
      if (socks_seteuid(&euid, sockscf.uid.unprivileged) != 0
      ||  socks_seteuid(NULL, euid)                      != 0)
         serr(EXIT_FAILURE, "%s: socks_seteuid() failed", function);
   }

#if HAVE_LIBWRAP
   if (!sockscf.uid.libwrap_isset) {
      sockscf.uid.libwrap       = sockscf.uid.unprivileged;
      sockscf.uid.libwrap_isset = 1;
   }
   else { /* check the euid-switching works. */
      if (socks_seteuid(&euid, sockscf.uid.libwrap) != 0
      ||  socks_seteuid(NULL, euid)                 != 0)
         serr(EXIT_FAILURE, "%s: socks_seteuid() failed", function);
   }
#endif /* HAVE_LIBWRAP */
#endif /* !HAVE_PRIVILEGES */
#endif /* !HAVE_DUMPCONF */

#if !HAVE_DUMPCONF && SOCKS_SERVER

   if (sockscf.clientmethodc == 0) {
      sockscf.clientmethodv[sockscf.clientmethodc++] = AUTHMETHOD_NONE;

      if (methodisset(AUTHMETHOD_GSSAPI, sockscf.methodv, sockscf.methodc))
         sockscf.clientmethodv[sockscf.clientmethodc++] = AUTHMETHOD_GSSAPI;
   }

   if (methodisset(AUTHMETHOD_GSSAPI, sockscf.methodv, sockscf.methodc)
   && !methodisset(AUTHMETHOD_GSSAPI, sockscf.clientmethodv,
   sockscf.clientmethodc))
      serrx(EXIT_FAILURE,
            "%s: authmethod %s is enabled for socks-methods, but not for "
            "client-methods.  Since %s authentication needs to be established "
            "during client-negotiation it thus needs to be set in "
            "clientmethods also",
            function, method2string(AUTHMETHOD_GSSAPI),
            method2string(AUTHMETHOD_GSSAPI));

   /*
    * Other way around should be ok since if the socks-rule method includes
    * "none", it shouldn't matter what auth-method was used during client
    * negotiation; none should be a subset of everything.
    */
#endif /* !HAVE_DUMPCONF && SOCKS_SERVER */

   if (sockscf.methodc == 0)
      swarnx("%s: no authentication methods enabled.  This means all requests "
             "will be blocked after negotiation.  Perhaps this is not "
             "intended?", function);


#if !HAVE_PRIVILEGES
   if (sockscf.uid.unprivileged == 0)
      swarnx("%s: setting the unprivileged uid to %d is not recommended "
             "for security reasons",
             function, sockscf.uid.unprivileged);

#if HAVE_LIBWRAP
   if (sockscf.uid.libwrap == 0)
      swarnx("%s: setting the libwrap uid to %d is not recommended "
             "for security reasons",
      function, sockscf.uid.libwrap);
#endif /* HAVE_LIBWRAP */
#endif /* !HAVE_PRIVILEGES */

   /*
    * Check rules, including if some rule-specific settings vary across
    * rules.  If they don't, we can optimize some things when running.
    */
   basec = 0;
   while (basec < ELEMENTS(basev)) {
      const int isclientrule = isclientrulev[basec];
      rule_t *rule = basev[basec];
      ++basec;

      if (rule == NULL)
         continue;

      for (; rule != NULL; rule = rule->next) {
         const command_t udpreplyonly = { .udpreply = 1 };

         for (i = 0; i < rule->state.methodc; ++i) {
            switch (rule->state.methodv[i]) {
#if HAVE_PAM
               case AUTHMETHOD_PAM:
                  if (sockscf.state.pamservicename == NULL)
                     break; /* already found to vary. */

                  if (pamservicename == NULL) /* first pam rule. */
                     pamservicename = rule->state.pamservicename;
                  else if (strcmp(pamservicename, rule->state.pamservicename)
                  != 0) {
                     slog(LOG_DEBUG, "%s: pam.servicename varies, %s ne %s",
                     function, pamservicename, rule->state.pamservicename);

                     sockscf.state.pamservicename = NULL;
                  }

                  break;
#endif /* HAVE_PAM */

#if HAVE_BSDAUTH
               case AUTHMETHOD_BSDAUTH:
                  if (sockscf.state.bsdauthstylename == NULL)
                     break; /* already found to vary. */

                  if (bsdauthstylename == NULL) /* first bsdauth rule. */
                     bsdauthstylename = rule->state.bsdauthstylename;
                  else if (strcmp(bsdauthstylename,
                  rule->state.bsdauthstylename) != 0) {
                     slog(LOG_DEBUG, "%s: bsdauth.stylename varies, %s ne %s",
                     function, bsdauthstylename, rule->state.bsdauthstylename);

                     sockscf.state.bsdauthstylename = NULL;
                  }

                  break;
#endif /* HAVE_BSDAUTH */

#if HAVE_GSSAPI
               case AUTHMETHOD_GSSAPI:
                  if (sockscf.state.gssapiservicename != NULL) {
                     if (gssapiservicename == NULL) /* first gssapi rule. */
                        gssapiservicename = rule->state.gssapiservicename;
                     else if (strcmp(gssapiservicename,
                     rule->state.gssapiservicename) != 0) {
                        slog(LOG_DEBUG,
                        "%s: gssapi.servicename varies, %s ne %s",
                         function, gssapiservicename,
                         rule->state.gssapiservicename);

                        sockscf.state.gssapiservicename = NULL;
                     }
                  }
                  /* else; already found to vary. */

                  if (sockscf.state.gssapikeytab != NULL) {
                     if (gssapikeytab == NULL) /* first gssapi rule. */
                        gssapikeytab = rule->state.gssapikeytab;
                     else if (strcmp(gssapikeytab, rule->state.gssapikeytab)
                     != 0) {
                        slog(LOG_DEBUG, "%s: gssapi.keytab varies, %s ne %s",
                        function, gssapikeytab, rule->state.gssapikeytab);

                        sockscf.state.gssapikeytab = NULL;
                     }
                  }
                  /* else; already found to vary. */

                  break;
#endif /* HAVE_GSSAPI */

               default:
                  break;
            }
         }

         if (rule->state.methodc == 0) {
            if (isreplycommandonly(&rule->state.command)
            &&  !sockscf.srchost.checkreplyauth)
               /* don't require user to specify a method for reply-only rules */
               ;
            else
               serrx(EXIT_FAILURE,
                     "%s: %s-rule #%lu allows no authentication methods",
                     function,
                     isclientrule ? "client" : "socks",
                     (unsigned long)rule->number);
         }

         if (isreplycommandonly(&rule->state.command)) {
            for (i = 0; i < rule->state.methodc; ++i) {
               switch (rule->state.methodv[i]) {
                  case AUTHMETHOD_NONE:
                  case AUTHMETHOD_PAM:
                     break;

                  case AUTHMETHOD_RFC931:
                     if (memcmp(&rule->state.command, &udpreplyonly,
                     sizeof(udpreplyonly)) == 0) /* udp only. */
                        serrx(EXIT_FAILURE,
                              "%s: %s-rule #%lu specifies method %s, but this "
                              "method can not be provided by udp replies",
                              function,
                              isclientrule ? "client" : "socks",
                              (unsigned long)rule->number,
                              method2string(rule->state.methodv[i]));
                     break;

                  default:
                     serrx(EXIT_FAILURE,
                           "%s: %s-rule #%lu specifies method %s, but this "
                           "method can not be provided by replies",
                           function,
                           isclientrule ? "client" : "socks",
                           (unsigned long)rule->number,
                           method2string(rule->state.methodv[i]));
               }
            }
         }

         if (rule->user != NULL || rule->group != NULL) {
            if (memcmp(&rule->state.command, &udpreplyonly,
            sizeof(udpreplyonly)) == 0)
               serrx(EXIT_FAILURE, "%s-rule #%lu: udp replies can "
                                   "not provide any user/group information",
                                   isclientrule ? "client" : "socks",
                                   (unsigned long)rule->number);

            for (i = 0; i < rule->state.methodc; ++i) {
               if (methodcanprovide(rule->state.methodv[i], username))
                  break;

            if (i >= rule->state.methodc)
               serrx(EXIT_FAILURE,
                     "%s-rule #%lu specifies a user/group-name, "
                     "but no method that can provide it",
                     isclientrule ? "client" : "socks",
                     (unsigned long)rule->number);
            }
         }
#if BAREFOOTD
         if (isclientrule && rule->state.protocol.tcp)
            /*
             * Add all "to:" addresses to the list of internal interfaces;
             * barefootd doesn't use a separate "internal:" keyword for it.
             */
             addinternal(&rule->dst, SOCKS_TCP);
         else if (!isclientrule && rule->state.protocol.udp)
            sockscf.state.alludpbounced = 0;
#endif /* BAREFOOTD */
      }
   }

#if HAVE_PAM
   if (sockscf.state.pamservicename != NULL
   &&  pamservicename               != NULL)
      /*
       * pamservicename does not vary, but is not necessarily the
       * the same as sockscf.state.pamservicename (default).
       * If it is not, set sockscf.state.pamservicename to
       * what the user used in one or more of the rules, since
       * it is the same in all rules, i.e. making it that value
       * we use to make passworddbisunique() work as expected.
       *
       * Likewise for bsdauth, gssapi, etc.
      */

      if (strcmp(pamservicename, sockscf.state.pamservicename) != 0)
         sockscf.state.pamservicename = pamservicename;
#endif /* HAVE_PAM */

#if HAVE_BSDAUTH
   if (sockscf.state.bsdauthstylename != NULL
   &&  bsdauthstylename               != NULL)
      if (strcmp(bsdauthstylename, sockscf.state.bsdauthstylename) != 0)
         sockscf.state.bsdauthstylename = bsdauthstylename;
#endif /* HAVE_BSDAUTH */

#if HAVE_GSSAPI
   if (sockscf.state.gssapiservicename != NULL
   &&  gssapiservicename               != NULL)
      if (strcmp(gssapiservicename, sockscf.state.gssapiservicename) != 0)
         sockscf.state.gssapiservicename = gssapiservicename;

   if (sockscf.state.gssapikeytab != NULL
   &&  gssapikeytab               != NULL)
      if (strcmp(gssapikeytab, sockscf.state.gssapikeytab) != 0)
         sockscf.state.gssapikeytab = gssapikeytab;
#endif /* HAVE_GSSAPI */

   /*
    * Go through all rules again and set default values for 
    * authentication-methods based on the global method-lines, if none set.
    */
   basec = 0;
   while (basec < ELEMENTS(basev)) {
#if HAVE_PAM || HAVE_BSDAUTH || HAVE_GSSAPI
      const int *methodv     = methodbasev[basec];
      const int methodc      = *methodbasec[basec];
#endif /* HAVE_PAM || HAVE_BSDAUTH || HAVE_GSSAPI */
      rule_t *rule = basev[basec];
      ++basec;

      if (rule == NULL)
         continue;

      for (; rule != NULL; rule = rule->next) {
#if HAVE_PAM
         if (methodisset(AUTHMETHOD_PAM, methodv, methodc))
            if (*rule->state.pamservicename == NUL) { /* set to default. */
               SASSERTX(strlen(sockscf.state.pamservicename)
               < sizeof(rule->state.pamservicename));

               strcpy(rule->state.pamservicename, sockscf.state.pamservicename);
            }
#endif /* HAVE_PAM */

#if HAVE_BSDAUTH
         if (methodisset(AUTHMETHOD_BSDAUTH, methodv, methodc))
            if (*rule->state.bsdauthstylename == NUL) { /* set to default. */
               if (sockscf.state.bsdauthstylename != NULL) {
                   SASSERTX(strlen(sockscf.state.bsdauthstylename)
                   < sizeof(rule->state.bsdauthstylename));

                   strcpy(rule->state.bsdauthstylename,
                   sockscf.state.bsdauthstylename);
               } else
                   rule->state.bsdauthstylename[0] = NUL;
            }
#endif /* HAVE_BSDAUTH */

#if HAVE_GSSAPI
         if (methodisset(AUTHMETHOD_GSSAPI, methodv, methodc)) {
            if (*rule->state.gssapiservicename == NUL) { /* set to default. */
               SASSERTX(strlen(sockscf.state.gssapiservicename)
               < sizeof(rule->state.gssapiservicename));

               strcpy(rule->state.gssapiservicename,
                      sockscf.state.gssapiservicename);
            }

            if (*rule->state.gssapikeytab == NUL) { /* set to default. */
               SASSERTX(strlen(sockscf.state.gssapikeytab)
               < sizeof(rule->state.gssapikeytab));
               strcpy(rule->state.gssapikeytab, sockscf.state.gssapikeytab);
            }

            /*
             * can't do memcmp since we don't want to include
             * gssapiencryption.nec in the compare.
             */
            if (rule->state.gssapiencryption.clear           == 0
            &&  rule->state.gssapiencryption.integrity       == 0
            &&  rule->state.gssapiencryption.confidentiality == 0
            &&  rule->state.gssapiencryption.permessage      == 0) {
               rule->state.gssapiencryption.clear          = 1;
               rule->state.gssapiencryption.integrity      = 1;
               rule->state.gssapiencryption.confidentiality= 1;
               rule->state.gssapiencryption.permessage     = 0;
            }
         }
#endif /* HAVE_GSSAPI */

#if HAVE_LDAP
         if (*rule->state.ldap.keytab == NUL) { /* set to default. */
            SASSERTX(sizeof(DEFAULT_GSSAPIKEYTAB)
            <= sizeof(rule->state.ldap.keytab));
            strcpy(rule->state.ldap.keytab, DEFAULT_GSSAPIKEYTAB);
         }

         if (*rule->state.ldap.filter == NUL) { /* set to default. */
            SASSERTX(sizeof(DEFAULT_LDAP_FILTER)
            <= sizeof(rule->state.ldap.filter));
            strcpy(rule->state.ldap.filter, DEFAULT_LDAP_FILTER);
         }

         if (*rule->state.ldap.filter_AD == NUL) { /* set to default. */
            SASSERTX(sizeof(DEFAULT_LDAP_FILTER_AD)
            <= sizeof(rule->state.ldap.filter_AD));
            strcpy(rule->state.ldap.filter_AD, DEFAULT_LDAP_FILTER_AD);
         }

         if (*rule->state.ldap.attribute == NUL) { /* set to default. */
            SASSERTX(sizeof(DEFAULT_LDAP_ATTRIBUTE)
            <= sizeof(rule->state.ldap.attribute));
            strcpy(rule->state.ldap.attribute, DEFAULT_LDAP_ATTRIBUTE);
         }

         if (*rule->state.ldap.attribute_AD == NUL) { /* set to default. */
            SASSERTX(sizeof(DEFAULT_LDAP_ATTRIBUTE_AD)
            <= sizeof(rule->state.ldap.attribute_AD));
            strcpy(rule->state.ldap.attribute_AD, DEFAULT_LDAP_ATTRIBUTE_AD);
         }

         if (*rule->state.ldap.certfile == NUL) { /* set to default. */
            SASSERTX(sizeof(DEFAULT_LDAP_CACERTFILE)
            <= sizeof(rule->state.ldap.certfile));
            strcpy(rule->state.ldap.certfile, DEFAULT_LDAP_CACERTFILE);
         }

         if (*rule->state.ldap.certpath == NUL) { /* set to default. */
            SASSERTX(sizeof(DEFAULT_LDAP_CERTDBPATH)
            <= sizeof(rule->state.ldap.certpath));
            strcpy(rule->state.ldap.certpath, DEFAULT_LDAP_CERTDBPATH);
         }

         if (rule->state.ldap.port == 0) /* set to default */
            rule->state.ldap.port = SOCKD_EXPLICIT_LDAP_PORT;

         if (rule->state.ldap.portssl == 0) /* set to default */
            rule->state.ldap.portssl = SOCKD_EXPLICIT_LDAPS_PORT;
#endif /* HAVE_LDAP */
      }
   }

   if (sockscf.internalc == 0
#if BAREFOOTD
   && sockscf.state.alludpbounced
#endif /* BAREFOOTD */
   )
      serrx(EXIT_FAILURE,
            "%s: no internal address given for server to listen for "
            "clients on",
            function);


   if (sockscf.external.addrc == 0)
      serrx(EXIT_FAILURE,
            "%s: no external address given for server to use forwarding "
            "data on behalf of clients",
            function);

   if (sockscf.external.rotation == ROTATION_SAMESAME
   &&  sockscf.external.addrc    == 1)
      swarnx("%s: rotation for external addresses is set to same-same, but "
             "the number of external addresses is only one, so this does "
             "not make sense",
             function);

   if (sockscf.routeoptions.maxfail == 0 && sockscf.routeoptions.badexpire != 0)
      swarnx("%s: it does not make sense to set \"route.badexpire\" "
             "when \"route.maxfail\" is set to zero",
             function);

#if COVENANT
   if (*sockscf.realmname == NUL)
      strcpy(sockscf.realmname, DEFAULT_REALMNAME);
#endif /* COVENANT */

#if HAVE_SCHED_SETAFFINITY
{
   const cpusetting_t *cpuv[] = { &sockscf.cpu.mother,
                                  &sockscf.cpu.negotiate,
                                  &sockscf.cpu.request,
                                  &sockscf.cpu.io };

   const int proctypev[]      = { CHILD_MOTHER,
                                  CHILD_NEGOTIATE,
                                  CHILD_REQUEST,
                                  CHILD_IO };
   size_t i;

   for (i = 0; i < ELEMENTS(cpuv); ++i)
   if (cpuv[i]->affinity_isset && !sockd_cpuset_isok(&cpuv[i]->mask))
      serrx(EXIT_FAILURE,
            "%s: invalid cpu mask configured for %s process: %s",
            function,
            childtype2string(proctypev[i]),
            cpuset2string(&cpuv[i]->mask, NULL, 0));
}
#endif /* HAVE_SCHED_SETAFFINITY */


#if !HAVE_DUMPCONF
   if (pidismother(sockscf.state.pid) == 1) {   /* main mother */
      for (i = 0; i < sockscf.external.addrc; ++i)
         if (!addrisbindable(&sockscf.external.addrv[i]))
            serrx(EXIT_FAILURE, "%s: can not bind external address #%ld: %s",
                  function,
                  (long)i,
                  ruleaddr2string(&sockscf.external.addrv[i], NULL, 0));
   }
#endif /* !HAVE_DUMPCONF */

   if (sockscf.timeout.tcp_fin_wait == 0
   ||  sockscf.timeout.tcp_fin_wait  > sockscf.timeout.tcpio)
      sockscf.timeout.tcp_fin_wait = sockscf.timeout.tcpio;
}

static void
log_clientsend(protocol, child, isresend)
   const int protocol;
   const sockd_child_t *child;
   const int isresend;
{

   slog(LOG_DEBUG, "%s %s-client to %s-child %lu, %lu slots free",
                   isresend ? "trying again to send" : "sending",
                   protocol2string(protocol),
                   childtype2string(child->type),
                   (unsigned long)child->pid,
                   (unsigned long)child->freec);
}

static void
log_probablytimedout(client, childtype)
   const struct sockaddr *client;
   const int childtype;
{

   swarn("client %s probably timed out waiting for us to send it to a %s-child",
         sockaddr2string(client, NULL, 0), childtype2string(childtype));
}

static void
log_noclientrecv(prefix, child)
   const char *prefix;
   const sockd_child_t *child;
{

   slog(LOG_DEBUG, "%s.  Not trying to receive a new client object now from "
                   "%s-child %u",
                   prefix, childtype2string(child->type), (unsigned)child->pid);
}



static void
handlechildcommand(command, child, finished)
   const unsigned char command;
   sockd_child_t *child;
   int *finished;
{
   const char *function = "handlechildcommand()";

   slog(LOG_DEBUG, "%s: command %d from %s-child %lu",
        function,
        command,
        childtype2string(child->type),
        (unsigned long)child->pid);

   switch(command) {
      case SOCKD_NOP:
         break;

      case SOCKD_FREESLOT_TCP:
      case SOCKD_FREESLOT_UDP:
         ++child->freec;

         slog(LOG_DEBUG, "%s: %s-child %lu has freed a %s slot, "
                         "now has %lu slot%s free",
                         function,
                         childtype2string(child->type),
                         (unsigned long)child->pid,
                         command == SOCKD_FREESLOT_TCP ?
                         "tcp" : "udp",
                         (unsigned long)child->freec,
                         child->freec == 1 ? "" : "s");

         SASSERTX(child->freec <= maxfreeslots(child->type));

         if (child->type == CHILD_IO) {
            /*
             * don't receive anything back from i/o childs
             * except the freeslot ack, as i/o childs are the
             * last in the chain, so need to update this stat her.
             */
            ++sockscf.stat.io.received;
#if COVENANT
#warning   "does not always apply to covenant"
#endif /* COVENANT */

#if BAREFOOTD
            if (command == SOCKD_FREESLOT_UDP) {
               --child->hasudpsession;
               SASSERTX(child->hasudpsession == 0);
            }
#endif /* BAREFOOTD */
         }

         break;

      default:
         SERRX(command);
   }

   if (sockscf.child.maxrequests != 0
   &&  child->freec              == maxfreeslots(child->type)
   &&  child->sentc              >= sockscf.child.maxrequests) {
      slog(LOG_DEBUG, "should close  connection to %s-child %lu as it "
                      "has now handled %lu request%s",
                      childtype2string(child->type),
                      (unsigned long)child->pid,
                      (unsigned long)child->sentc,
                      (unsigned long)child->sentc == 1 ? "" : "s");
      *finished = 1;
   }
   else
      *finished = 0;
}

static char *
getlimitinfo(void)
{
#ifndef RLIMIT_NPROC
   return "";
#else /* have RLIMIT_NPROC */
   const char *function = "getlimitinfo()";
   static char buf[2048];
   const int fds_per_proc = 2; /* two pipes */
   const char *limiter;
   struct rlimit maxfd, maxproc;
   char maxprocstr[64], maxfdstr[64];
   unsigned long negc_proc, negc_fd, reqc_proc, reqc_fd, ioc_proc, ioc_fd,
                 negc_limit, reqc_limit, ioc_limit,
                 proc_free, proc_used, procs, fds_free;

   if (getrlimit(RLIMIT_NOFILE, &maxfd) != 0) {
      swarn("%s: getrlimit(RLIMIT_NOFILE) failed", function);
      return "";
   }

   if (getrlimit(RLIMIT_NPROC, &maxproc) != 0) {
      swarn("%s: getrlimit(RLIMIT_NPROC) failed", function);
      return "";
   }

   if (maxfd.rlim_cur   == RLIM_INFINITY
   &&  maxproc.rlim_cur == RLIM_INFINITY)
      return "no applicable environment resource limits configured";

   proc_used   = sockscf.option.serverc
               + childcheck(-CHILD_NEGOTIATE) / SOCKD_NEGOTIATEMAX
               + childcheck(-CHILD_REQUEST)   / SOCKD_REQUESTMAX
               + childcheck(-CHILD_IO)        / SOCKD_IOMAX;
   proc_free   = maxproc.rlim_cur - proc_used;

   if (maxproc.rlim_cur == RLIM_INFINITY)
      snprintf(maxprocstr, sizeof(maxprocstr), "no limit");
   else
      snprintf(maxprocstr, sizeof(maxprocstr),
               "%lu (%lu free)",
               (unsigned long)maxproc.rlim_cur, proc_free);

   fds_free = freedescriptors(NULL) - FDPASS_MAX;
   if (maxfd.rlim_cur == RLIM_INFINITY)
      snprintf(maxfdstr, sizeof(maxfdstr), "no limit");
   else
      snprintf(maxfdstr, sizeof(maxfdstr),
               "%lu (%lu free)", (unsigned long)maxfd.rlim_cur, fds_free);

   /*
    * Calculate the max number of new clients we can handle based on both
    * the process resource limit and the fd limit.
    */


   /*
    * Process-based limit, disregarding any other limits.
    * Each process can handle SOCKD_{NEGOTIATE,REQUEST,IO}MAX clients.
    * We can create a max number of proc_free additional processes, so
    * the number of additional clients we can handle is the number
    * of additional clients multiplied by the number of clients each
    * process can handle.
    */
   negc_proc = proc_free * SOCKD_NEGOTIATEMAX;
   reqc_proc = proc_free * SOCKD_REQUESTMAX;
   ioc_proc  = proc_free * SOCKD_IOMAX;

   /*
    * FD-based limit, disregarding any other limits.
    * With the fds we have, we can create a given number of additional
    * processes (procs).
    * Each process needs fds_per_proc, and an additional
    * SOCKD_{NEGOTIATE,REQUEST,IO}MAX * <number of fds per client in this
    * phase> fds to handle the max number of clients, meaning we can handle
    * the following number of additional clients:
    */
   procs   = fds_free / fds_per_proc;
   negc_fd = MIN(((fds_free - fds_per_proc) / 1), SOCKD_NEGOTIATEMAX)
           * procs;
   reqc_fd = MIN(((fds_free - fds_per_proc) / FDPASS_MAX), SOCKD_REQUESTMAX)
           * procs;
   ioc_fd  = MIN(((fds_free - fds_per_proc) / FDPASS_MAX), SOCKD_IOMAX)
           * procs;

   /*
    * Different process-types could be limited by different things, but
    * ignore that here.
    */
   if (negc_proc < negc_fd
   ||  reqc_proc < reqc_fd
   ||  ioc_proc  < ioc_fd) {
      limiter = "process";

      negc_limit = negc_proc;
      reqc_limit = reqc_proc;
      ioc_limit  = ioc_proc;
   }
   else {
      limiter = "open file";

      negc_limit = negc_fd;
      reqc_limit = reqc_fd;
      ioc_limit  = ioc_fd;
   }

   snprintf(buf, sizeof(buf), "max limits: process: %s, file: %s, "
            "neg: %lu, req: %lu, io: %lu (clients limited by %s limit)",
            maxprocstr, maxfdstr, negc_limit, reqc_limit, ioc_limit, limiter);

   return buf;
#endif /* have RLIMIT_NPROC */
}

#if DEBUG && 0
static void
dotest(void)
{
   const char *function = "dotest()";
   sockd_child_t *child;
   sockd_client_t client;
   sockd_request_t request;
   sockd_io_t io;
   int i;

   slog(LOG_INFO, "%s: starting send_client() test ...", function);

   if ((child = nextchild(CHILD_NEGOTIATE, SOCKS_TCP)) == NULL)
      serr(EXIT_FAILURE, "%s: nextchild(CHILD_NEGOTIATE) failed", function);

   if (kill(child->pid, SIGSTOP) != 0)
      serr(EXIT_FAILURE, "%s: kill(SIGSTOP) of child %ld failed",
      function, (long)child->pid);

   bzero(&client, sizeof(client));
   if ((client.s = socket(AF_INET, SOCK_STREAM, 0)) == -1)
      serr(EXIT_FAILURE, "%s: failed to create a SOCK_STREAM socket", function);

   i = 0;
   while (send_client(child->s, &client, NULL, 0) == 0)
      ++i;

   if (kill(child->pid, SIGTERM) != 0)
      serr(EXIT_FAILURE, "%s: kill(SIGTERM) of child %ld failed",
      function, (long)child->pid);

   if (i >= SOCKD_NEGOTIATEMAX)
      slog(LOG_INFO, "%s: send_client() test completed ok, sent %d requests",
      function, i);
   else
      swarn("%s: send_client() test failed after %d requests", function, i);


   slog(LOG_INFO, "%s: starting send_req() test ...", function);

   if ((child = nextchild(CHILD_REQUEST, SOCKS_TCP)) == NULL)
      serr(EXIT_FAILURE, "%s: nextchild(CHILD_REQUEST) failed", function);

   if (kill(child->pid, SIGSTOP) != 0)
      serr(EXIT_FAILURE, "%s: kill(SIGSTOP) of child %ld failed",
      function, (long)child->pid);

   bzero(&request, sizeof(request));
   if ((request.s = socket(AF_INET, SOCK_STREAM, 0)) == -1)
      serr(EXIT_FAILURE, "%s: failed to create a SOCK_STREAM socket", function);

   i = 0;
   while (send_req(child->s, &request) == 0)
      ++i;

   if (kill(child->pid, SIGTERM) != 0)
      serr(EXIT_FAILURE, "%s: kill(SIGTERM) of child %ld failed",
      function, (long)child->pid);

   if (i >= SOCKD_REQUESTMAX)
      slog(LOG_INFO, "%s: send_req() test completed ok, sent %d requests",
      function, i);
   else
      swarn("%s: send_req() test failed after %d requests", function, i);

   slog(LOG_INFO, "%s: starting send_io() test ...", function);

   if ((child = nextchild(CHILD_IO, SOCKS_TCP)) == NULL)
      serr(EXIT_FAILURE, "%s: nextchild(CHILD_IO) failed", function);

   if (kill(child->pid, SIGSTOP) != 0)
      serr(EXIT_FAILURE, "%s: kill(SIGSTOP) of child %ld failed",
      function, (long)child->pid);

   bzero(&io, sizeof(io));
   io.state.command = SOCKS_UDPASSOCIATE;
   if ((io.control.s = socket(AF_INET, SOCK_STREAM, 0)) == -1
   ||  (io.src.s     = socket(AF_INET, SOCK_STREAM, 0)) == -1
   ||  (io.dst.s     = socket(AF_INET, SOCK_DGRAM, 0)) == -1)
      serr(EXIT_FAILURE, "%s: failed to create a SOCK_STREAM socket", function);

   i = 0;
   while (send_io(child->s, &io) == 0)
      ++i;

   if (kill(child->pid, SIGTERM) != 0)
      serr(EXIT_FAILURE, "%s: kill(SIGTERM) of child %ld failed",
      function, (long)child->pid);

   if (i >= SOCKD_IOMAX)
      slog(LOG_INFO, "%s: send_io() test completed ok, sent %d requests",
      function, i);
   else
      swarn("%s: send_io() test failed after %d requests", function, i);

#if 0
   socks_iobuftest();
#endif
}

#endif /* DEBUG */
