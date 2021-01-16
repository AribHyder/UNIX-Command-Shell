/* 
 * tsh - A tiny shell program with job control
 * 
 * <Agha Arib Hyder : aah5469@psu.edu>
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
  pid_t pid;              /* job PID */
  int jid;                /* job ID [1, 2, ...] */
  int state;              /* UNDEF, BG, FG, or ST */
  char cmdline[MAXLINE];  /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */
/* End global variables */


/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs); 
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid); 
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine 
 */
int main(int argc, char **argv) 
{
  char c;
  char cmdline[MAXLINE];
  int emit_prompt = 1; /* emit prompt (default) */

  /* Redirect stderr to stdout (so that driver will get all output
   * on the pipe connected to stdout) */
  dup2(1, 2);

  /* Parse the command line */
  while ((c = getopt(argc, argv, "hvp")) != EOF) {
    switch (c) {
      case 'h':             /* print help message */
        usage();
        break;
      case 'v':             /* emit additional diagnostic info */
        verbose = 1;
        break;
      case 'p':             /* don't print a prompt */
        emit_prompt = 0;  /* handy for automatic testing */
        break;
      default:
        usage();
    }
  }

  /* Install the signal handlers */

  /* These are the ones you will need to implement */
  Signal(SIGINT,  sigint_handler);   /* ctrl-c */
  Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
  Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */

  /* This one provides a clean way to kill the shell */
  Signal(SIGQUIT, sigquit_handler); 

  /* Initialize the job list */
  initjobs(jobs);

  /* Execute the shell's read/eval loop */
  while (1) {

    /* Read command line */
    if (emit_prompt) {
      printf("%s", prompt);
      fflush(stdout);
    }
    if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
      app_error("fgets error");
    if (feof(stdin)) { /* End of file (ctrl-d) */
      fflush(stdout);
      exit(0);
    }

    /* Evaluate the command line */
    eval(cmdline);
    fflush(stdout);
    fflush(stdout);
  } 

  exit(0); /* control never reaches here */
}

/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
 */
void eval(char *cmdline) 
{
  //argument list for execve()
  char *argv[MAXARGS];
  //holds command line with max characters
  char buf[MAXLINE];
  //process id
  pid_t pid;
  //used for blocking and unblocking signals
  sigset_t sigblock;
  //determines if job should be fg or bg job
  int bg;
  //copy input to buf (modified command line) and call parseline function
  strcpy(buf, cmdline);

  bg = parseline(buf, argv);
  //returns if no input comes from the user
  if (argv[0] == NULL) {
    return;
  }
  //error handling for SIGCHLD, SIGINT, and SIGTSTP with sigaddset
  if (sigaddset(&sigblock, (SIGCHLD || SIGINT || SIGTSTP)) < 0) {
    unix_error("ERROR");
  }
  //control passes here if the given command is not a built-in command
  if(!builtin_cmd(argv)) {
    //
    if (sigprocmask(SIG_BLOCK, &sigblock, NULL)) {
      unix_error("ERROR");
    }
    //adding and blocking SIGCHLD
    sigemptyset(&sigblock);
    sigaddset(&sigblock, SIGCHLD);
    sigprocmask(SIG_BLOCK, &sigblock, NULL);
    //fork new child for the process
    if ((pid = fork()) == 0) {
      //unblock the SIGCHLD
      sigprocmask(SIG_UNBLOCK, &sigblock, NULL);
      //generate a different process group ID for child process
      setpgid(0, 0);


      //error handling when the given command is not valid
      if (execve(argv[0], argv, environ) < 0) {
        printf("%s: Command not found\n", argv[0]);
        exit(0);
      }
    }
    //add job to job list based on whether it is FG or BG
    if (bg) {
      addjob(jobs, pid, BG, cmdline);
    }
    else if (!bg) {
      addjob(jobs, pid, FG, cmdline);
    }
    //unblock SIGCHLD
    sigprocmask(SIG_UNBLOCK, &sigblock, NULL);
    //print out BG job info if BG, and wait for FG to complete if FG
    if (bg) {
      printf("[%d] (%d) %s", pid2jid(pid), pid, cmdline);
    }
    else if (!bg) {
      waitfg(pid);
    }
  }

  return;
}

/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.  
 */
int parseline(const char *cmdline, char **argv) 
{
  static char array[MAXLINE]; /* holds local copy of command line */
  char *buf = array;          /* ptr that traverses command line */
  char *delim;                /* points to first space delimiter */
  int argc;                   /* number of args */
  int bg;                     /* background job? */

  strcpy(buf, cmdline);
  buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */
  while (*buf && (*buf == ' ')) /* ignore leading spaces */
    buf++;

  /* Build the argv list */
  argc = 0;
  if (*buf == '\'') {
    buf++;
    delim = strchr(buf, '\'');
  }
  else {
    delim = strchr(buf, ' ');
  }

  while (argc < MAXARGS-1 && delim) {
    argv[argc++] = buf;
    *delim = '\0';
    buf = delim + 1;
    while (*buf && (*buf == ' ')) /* ignore spaces */
      buf++;

    if (*buf == '\'') {
      buf++;
      delim = strchr(buf, '\'');
    }
    else {
      delim = strchr(buf, ' ');
    }
  }
  if (delim) {
    fprintf(stderr, "Too many arguments.\n");
    argc = 0; //treat it as an empty line.
  }
  argv[argc] = NULL;

  if (argc == 0)  /* ignore blank line */
    return 1;

  /* should the job run in the background? */
  if ((bg = (*argv[argc-1] == '&')) != 0) {
    argv[--argc] = NULL;
  }
  return bg;
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.  
 */
int builtin_cmd(char **argv) 
{
  //process quit command
  if (!strcmp(argv[0], "quit")) {
    exit(0);
  }
  //process fg job
  if (!strcmp(argv[0], "fg")) {
    do_bgfg(argv);
    return 1;
  }
  //process bg job
  if (!strcmp(argv[0], "bg")) {
    do_bgfg(argv);
    return 1;
  }
  //return job list
  if (!strcmp(argv[0], "jobs")) {
    listjobs(jobs);
    return 1;
  }
  //return nothing if it is not a built-in command
  return 0;
}

/* 
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv) 
{
  //store process id
  pid_t pid;
  //store job id
  int jid;
  //store the current job
  struct job_t *currentJob;
  //check if second argument is given, otherwise return an error
  if (argv[1] == NULL) {
    printf("%s command requires PID or %%jobid argument\n", argv[0]);
    return;
  }
  //control passes here if second command holds jid value
  if (argv[1][0] == '%') {
    //get the jid value
    jid = atoi(&(argv[1])[1]);
    //get the current job
    currentJob = getjobjid(jobs, jid);
    //get pid if job is not NULL, otherwise throw an error if it is
    if (currentJob != NULL) {
      pid = currentJob -> pid;
    }
    else {
      printf("%s: No such job\n", argv[1]);
      return;
    }
  }
  //control passes here if the second argument specifies a pid value
  else if (isdigit(argv[1][0])) {
    //get pid value
    pid = atoi(argv[1]);
    //get current job
    currentJob = getjobjid(jobs, pid);
    //if current job is NULL, return with an error
    if (currentJob == NULL) {
      printf("(%d): No such process\n", pid);
      return;
    }
  }
  //if the second argument does not specify jid or pid value, return an error
  else {
    printf("%s: argument must be a PID or %%jobid\n", argv[0]);
    return;
  }
  //kill current pid and send SIGCONT signal
  kill(-pid, SIGCONT);
  //processing for a bg job
  if (strcmp("bg", argv[0])) {
    //set current job's state to FG
    currentJob -> state = FG;
    //call waitfg for job to process
    waitfg(currentJob -> pid);
  }
  //processing for a fg job
  else if (strcmp("fg", argv[0])) {
    //set the current job's state to BG
    currentJob -> state = BG;
    //print out the current state of the job
    printf("[%d] (%d) %s", currentJob -> jid, currentJob -> pid, currentJob -> cmdline);
  }
  //return error if pid or jid value not specified
  else {
    printf("%s: argument must be a PID or %%jobid\n", argv[0]);
  }

  return;
}

/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid)
{
  //fgJob holds the FG job in a job struct
  struct job_t *fgJob;
  fgJob = getjobpid(jobs, pid);
  //return nothing if no pid is specified
  if (!pid) {
    return;
  }
  //sleep through the duration of the FG job processing
  while (fgJob -> state == FG) {
    if (fgJob -> pid == pid) {
      sleep(1);
    }
  }
  return;
}

/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.  
 */
void sigchld_handler(int sig) 
{
  //store status 
  int status;
  //store pid
  pid_t pid;
  //store jid
  int jid;
  //struct to hold current job
  struct job_t *currentJob;

  //while loop to reap any zombie child processes
  while((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
    //get jid and job
    jid = pid2jid(pid);
    currentJob = getjobpid(jobs,pid);
    //return with an error if job doesn't exist
    if (currentJob == NULL) {
      printf("(%d): No such job", pid);
      return;
    }
    //if child exited or terminated, delete the job
    if (WIFEXITED(status) || WIFSIGNALED(status)) {
      deletejob(jobs, pid);
      //print status if the child terminated
      if (WIFSIGNALED(status)) {
        printf("Job [%d] (%d) terminated by signal %d\n", jid, pid, WTERMSIG(status));
      }
    }
    //if child stopped, set current job status to ST and print out job info
    if (WIFSTOPPED(status)) {
      currentJob -> state = ST;
      printf("Job [%d] (%d) stopped by signal %d\n", jid, pid, WSTOPSIG(status));
    }
  }
  return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig) 
{
  //retrieve pid value of the job
  pid_t pid = fgpid(jobs);
  //if pid is not NULL send SIGINT signal
  if (pid) {   
    kill(-pid, SIGINT);
  }
  return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig) 
{
  //retrieve pid value of the job
  pid_t pid = fgpid(jobs);
  //if pid is not NULL send SIGTSTP signal
  if (pid) {
    kill(-pid, SIGTSTP);	
  }

  return;
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
  job->pid = 0;
  job->jid = 0;
  job->state = UNDEF;
  job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
  int i;

  for (i = 0; i < MAXJOBS; i++)
    clearjob(&jobs[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs) 
{
  int i, max=0;

  for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].jid > max)
      max = jobs[i].jid;
  return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) 
{
  int i;

  if (pid < 1)
    return 0;

  for (i = 0; i < MAXJOBS; i++) {
    if (jobs[i].pid == 0) {
      jobs[i].pid = pid;
      jobs[i].state = state;
      jobs[i].jid = nextjid++;
      if (nextjid > MAXJOBS)
        nextjid = 1;
      strcpy(jobs[i].cmdline, cmdline);
      if(verbose){
        printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
      }
      return 1;
    }
  }
  printf("Tried to create too many jobs\n");
  return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) 
{
  int i;

  if (pid < 1)
    return 0;

  for (i = 0; i < MAXJOBS; i++) {
    if (jobs[i].pid == pid) {
      clearjob(&jobs[i]);
      nextjid = maxjid(jobs)+1;
      return 1;
    }
  }
  return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
  int i;

  for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].state == FG)
      return jobs[i].pid;
  return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
  int i;

  if (pid < 1)
    return NULL;
  for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].pid == pid)
      return &jobs[i];
  return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
  int i;

  if (jid < 1)
    return NULL;
  for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].jid == jid)
      return &jobs[i];
  return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) 
{
  int i;

  if (pid < 1)
    return 0;
  for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].pid == pid) {
      return jobs[i].jid;
    }
  return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs) 
{
  int i;

  for (i = 0; i < MAXJOBS; i++) {
    if (jobs[i].pid != 0) {
      printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
      switch (jobs[i].state) {
        case BG: 
          printf("Running ");
          break;
        case FG: 
          printf("Foreground ");
          break;
        case ST: 
          printf("Stopped ");
          break;
        default:
          printf("listjobs: Internal error: job[%d].state=%d ", 
              i, jobs[i].state);
      }
      printf("%s", jobs[i].cmdline);
    }
  }
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void) 
{
  printf("Usage: shell [-hvp]\n");
  printf("   -h   print this message\n");
  printf("   -v   print additional diagnostic information\n");
  printf("   -p   do not emit a command prompt\n");
  exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg)
{
  fprintf(stdout, "%s: %s\n", msg, strerror(errno));
  exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg)
{
  fprintf(stdout, "%s\n", msg);
  exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) 
{
  struct sigaction action, old_action;

  action.sa_handler = handler;  
  sigemptyset(&action.sa_mask); /* block sigs of type being handled */
  action.sa_flags = SA_RESTART; /* restart syscalls if possible */

  if (sigaction(signum, &action, &old_action) < 0)
    unix_error("Signal error");
  return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig) 
{
  printf("Terminating after receipt of SIGQUIT signal\n");
  exit(1);
}



