
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <errno.h>
#include <ctype.h>

#define MAX_INPUT   2048
#define MAX_ARGS    128
#define MAX_CMDS    32         // max piped segments
#define MAX_JOBS    64
#define MAX_HISTORY 200

typedef enum { JOB_RUNNING=0, JOB_STOPPED=1, JOB_DONE=2 } job_state;

typedef struct {
    int   id;                  // 1-based job id
    pid_t pgid;                // process group id (pipeline group)
    char  cmdline[MAX_INPUT];
    job_state state;
} Job;

static Job jobs[MAX_JOBS];
static int  job_count = 0;
static int  next_job_id = 1;

static char *history[MAX_HISTORY];
static int   history_len = 0;

// ---------- helpers ----------
static void trim(char *s) {
    // trim leading/trailing spaces
    int n = (int)strlen(s);
    int i = 0; while (isspace((unsigned char)s[i])) i++;
    int j = n-1; while (j>=i && isspace((unsigned char)s[j])) j--;
    int len = j-i+1;
    if (len < 0) len = 0;
    memmove(s, s+i, len);
    s[len] = '\0';
}

static void add_history(const char *line) {
    if (!line || !*line) return;
    if (history_len == MAX_HISTORY) {
        free(history[0]);
        memmove(&history[0], &history[1], sizeof(char*)*(MAX_HISTORY-1));
        history_len--;
    }
    history[history_len++] = strdup(line);
}

static const char* history_path() {
    static char path[1024];
    const char *home = getenv("HOME");
    if (!home) home = ".";
    snprintf(path, sizeof(path), "%s/.myshell_history", home);
    return path;
}

static void load_history() {
    FILE *f = fopen(history_path(), "r");
    if (!f) return;
    char *line = NULL; size_t cap = 0;
    while (getline(&line, &cap, f) != -1) {
        line[strcspn(line, "\n")] = 0;
        add_history(line);
    }
    free(line);
    fclose(f);
}

static void save_history() {
    FILE *f = fopen(history_path(), "w");
    if (!f) return;
    for (int i=0;i<history_len;i++) fprintf(f, "%s\n", history[i]);
    fclose(f);
}

// ---------- job table ----------
static int find_job_index_by_id(int id) {
    for (int i=0;i<job_count;i++) if (jobs[i].id == id) return i;
    return -1;
}

static int find_job_index_by_pgid(pid_t pgid) {
    for (int i=0;i<job_count;i++) if (jobs[i].pgid == pgid) return i;
    return -1;
}

static void print_jobs() {
    for (int i=0;i<job_count;i++) {
        if (jobs[i].state == JOB_DONE) continue; // optional: hide done
        const char *st = jobs[i].state==JOB_RUNNING ? "Running" :
                         jobs[i].state==JOB_STOPPED ? "Stopped" : "Done";
        printf("[%d] %d  %-8s  %s\n", jobs[i].id, jobs[i].pgid, st, jobs[i].cmdline);
    }
}

static void remove_done_jobs() {
    int w = 0;
    for (int r=0;r<job_count;r++) {
        if (jobs[r].state != JOB_DONE) {
            if (w != r) jobs[w] = jobs[r];
            w++;
        }
    }
    job_count = w;
}

static void add_job_entry(pid_t pgid, const char *cmdline, job_state st) {
    if (job_count >= MAX_JOBS) return;
    jobs[job_count].id = next_job_id++;
    jobs[job_count].pgid = pgid;
    jobs[job_count].state = st;
    strncpy(jobs[job_count].cmdline, cmdline, sizeof(jobs[job_count].cmdline)-1);
    jobs[job_count].cmdline[sizeof(jobs[job_count].cmdline)-1] = '\0';
    job_count++;
}

// ---------- signals ----------
static void sigchld_handler(int sig) {
    (void)sig;
    int status;
    pid_t pid;
    // Reap all children; update job state
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
        pid_t pg = getpgid(pid);
        int idx = find_job_index_by_pgid(pg);
        if (idx >= 0) {
            if (WIFSTOPPED(status)) {
                jobs[idx].state = JOB_STOPPED;
            } else if (WIFCONTINUED(status)) {
                jobs[idx].state = JOB_RUNNING;
            } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
                jobs[idx].state = JOB_DONE;
            }
        }
    }
}

static void install_signal_handlers() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP | SA_NOCLDWAIT; // auto-reap zombies
    sigaction(SIGCHLD, &sa, NULL);

    // Ignore Ctrl-C in shell; children will reset to default
    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
}

// ---------- parsing ----------
typedef struct {
    char *argv[MAX_ARGS];
    char *infile;    // for '<'
    char *outfile;   // for '>' or '>>'
    int   append;    // 0:>, 1:>>
} Command;

// split pipeline into segments by '|'
static int split_pipeline(char *line, char *segments[], int maxseg) {
    int n = 0;
    char *saveptr = NULL;
    char *tok = strtok_r(line, "|", &saveptr);
    while (tok && n < maxseg) {
        while (*tok==' ') tok++;
        segments[n++] = tok;
        tok = strtok_r(NULL, "|", &saveptr);
    }
    return n;
}

static void parse_command(char *segment, Command *cmd) {
    memset(cmd, 0, sizeof(*cmd));
    int argc = 0;
    char *saveptr = NULL;
    char *tok = strtok_r(segment, " \t\n", &saveptr);
    while (tok && argc < MAX_ARGS-1) {
        if (strcmp(tok, "<") == 0) {
            tok = strtok_r(NULL, " \t\n", &saveptr);
            if (tok) cmd->infile = tok;
        } else if (strcmp(tok, ">") == 0) {
            tok = strtok_r(NULL, " \t\n", &saveptr);
            if (tok) { cmd->outfile = tok; cmd->append = 0; }
        } else if (strcmp(tok, ">>") == 0) {
            tok = strtok_r(NULL, " \t\n", &saveptr);
            if (tok) { cmd->outfile = tok; cmd->append = 1; }
        } else {
            cmd->argv[argc++] = tok;
        }
        tok = strtok_r(NULL, " \t\n", &saveptr);
    }
    cmd->argv[argc] = NULL;
}

// ---------- execution ----------
static int builtin_cd(char **argv) {
    const char *target = argv[1] ? argv[1] : getenv("HOME");
    if (!target) target = ".";
    if (chdir(target) != 0) perror("cd");
    return 0;
}

static void print_pwd() {
    char buf[4096];
    if (getcwd(buf, sizeof(buf))) puts(buf);
    else perror("pwd");
}

static bool is_builtin(const Command *cmd) {
    if (!cmd->argv[0]) return true;
    return strcmp(cmd->argv[0], "cd")==0 ||
           strcmp(cmd->argv[0], "pwd")==0 ||
           strcmp(cmd->argv[0], "exit")==0 ||
           strcmp(cmd->argv[0], "jobs")==0 ||
           strcmp(cmd->argv[0], "fg")==0   ||
           strcmp(cmd->argv[0], "bg")==0   ||
           strcmp(cmd->argv[0], "kill")==0 ||
           strcmp(cmd->argv[0], "history")==0;
}

static int run_builtin(Command *cmd, const char *full_line) {
    (void)full_line;
    char **argv = cmd->argv;
    if (!argv[0]) return 0;
    if (strcmp(argv[0], "cd")==0)          return builtin_cd(argv);
    if (strcmp(argv[0], "pwd")==0)         { print_pwd(); return 0; }
    if (strcmp(argv[0], "exit")==0)        { save_history(); exit(0); }
    if (strcmp(argv[0], "jobs")==0)        { remove_done_jobs(); print_jobs(); return 0; }
    if (strcmp(argv[0], "history")==0)     { for (int i=0;i<history_len;i++) printf("%d  %s\n", i+1, history[i]); return 0; }

    if (strcmp(argv[0], "fg")==0) {
        if (!argv[1]) { fprintf(stderr, "fg <job_id>\n"); return -1; }
        int id = atoi(argv[1]);
        int idx = find_job_index_by_id(id);
        if (idx<0) { fprintf(stderr, "fg: no such job\n"); return -1; }
        pid_t pgid = jobs[idx].pgid;
        // Give terminal to job, continue, wait, restore
        tcsetpgrp(STDIN_FILENO, pgid);
        kill(-pgid, SIGCONT);
        int status;
        while (waitpid(-pgid, &status, WUNTRACED) > 0) {
            if (WIFSTOPPED(status)) { jobs[idx].state = JOB_STOPPED; break; }
        }
        tcsetpgrp(STDIN_FILENO, getpgrp());
        if (kill(-pgid, 0) == -1 && errno == ESRCH) jobs[idx].state = JOB_DONE;
        return 0;
    }

    if (strcmp(argv[0], "bg")==0) {
        if (!argv[1]) { fprintf(stderr, "bg <job_id>\n"); return -1; }
        int id = atoi(argv[1]);
        int idx = find_job_index_by_id(id);
        if (idx<0) { fprintf(stderr, "bg: no such job\n"); return -1; }
        kill(-jobs[idx].pgid, SIGCONT);
        jobs[idx].state = JOB_RUNNING;
        return 0;
    }

    if (strcmp(argv[0], "kill")==0) {
        if (!argv[1]) { fprintf(stderr, "kill <job_id>\n"); return -1; }
        int id = atoi(argv[1]);
        int idx = find_job_index_by_id(id);
        if (idx<0) { fprintf(stderr, "kill: no such job\n"); return -1; }
        if (kill(-jobs[idx].pgid, SIGTERM) == -1) perror("kill");
        jobs[idx].state = JOB_DONE;
        return 0;
    }

    return -1;
}

static void setup_redirections(const Command *cmd) {
    if (cmd->infile) {
        int fd = open(cmd->infile, O_RDONLY);
        if (fd < 0) { perror("open <"); _exit(1); }
        dup2(fd, STDIN_FILENO);
        close(fd);
    }
    if (cmd->outfile) {
        int flags = O_WRONLY | O_CREAT | (cmd->append ? O_APPEND : O_TRUNC);
        int fd = open(cmd->outfile, flags, 0644);
        if (fd < 0) { perror("open >"); _exit(1); }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }
}

// Execute a (possibly piped) command line.
// If background==true, don't wait; add to jobs.
static void execute_line(char *line, bool background, const char *full_cmd_for_jobs) {
    // Split into pipeline segments
    char *segments[MAX_CMDS];
    int nseg = split_pipeline(line, segments, MAX_CMDS);
    if (nseg <= 0) return;

    // If single built-in without pipes, run in-process
    Command first;
    if (nseg == 1) {
        char tmp[MAX_INPUT]; strncpy(tmp, segments[0], sizeof(tmp)-1); tmp[sizeof(tmp)-1]=0;
        parse_command(tmp, &first);
        if (is_builtin(&first)) {
            run_builtin(&first, full_cmd_for_jobs);
            return;
        }
    }

    int pipes[MAX_CMDS-1][2];
    for (int i=0;i<nseg-1;i++) if (pipe(pipes[i]) == -1) { perror("pipe"); return; }

    pid_t pgid = 0;
    for (int i=0;i<nseg;i++) {
        // parse each segment fresh
        char tmp[MAX_INPUT]; strncpy(tmp, segments[i], sizeof(tmp)-1); tmp[sizeof(tmp)-1]=0;
        Command cmd; parse_command(tmp, &cmd);
        pid_t pid = fork();
        if (pid == -1) { perror("fork"); return; }
        if (pid == 0) {
            // child: process group, signals default
            signal(SIGINT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);

            if (pgid == 0) pgid = getpid();
            setpgid(0, pgid);
            if (!background && i==0) tcsetpgrp(STDIN_FILENO, pgid);

            // connect pipes
            if (i > 0) { // read from previous
                dup2(pipes[i-1][0], STDIN_FILENO);
            }
            if (i < nseg-1) { // write to next
                dup2(pipes[i][1], STDOUT_FILENO);
            }
            // close all pipe fds in child
            for (int k=0;k<nseg-1;k++) {
                close(pipes[k][0]); close(pipes[k][1]);
            }

            // redirections
            setup_redirections(&cmd);

            // exec (builtins inside pipeline need a subshell; we skip and only exec real commands)
            if (!cmd.argv[0]) _exit(0);
            execvp(cmd.argv[0], cmd.argv);
            perror("execvp");
            _exit(127);
        } else {
            // parent
            if (pgid == 0) pgid = pid;
            setpgid(pid, pgid);
        }
    }

    // parent: close all pipe fds
    for (int i=0;i<nseg-1;i++) { close(pipes[i][0]); close(pipes[i][1]); }

    if (background) {
        add_job_entry(pgid, full_cmd_for_jobs, JOB_RUNNING);
        printf("[%%%d] started in background, PGID=%d\n", jobs[job_count-1].id, pgid);
        // keep shell controlling terminal
        tcsetpgrp(STDIN_FILENO, getpgrp());
        return;
    }

    // Foreground: wait for the whole group
    int status;
    tcsetpgrp(STDIN_FILENO, pgid);
    while (waitpid(-pgid, &status, WUNTRACED) > 0) {
        if (WIFSTOPPED(status)) {
            int idx = find_job_index_by_pgid(pgid);
            if (idx < 0) add_job_entry(pgid, full_cmd_for_jobs, JOB_STOPPED);
            else jobs[idx].state = JOB_STOPPED;
            printf("\n[stopped] %s\n", full_cmd_for_jobs);
            break;
        }
    }
    // Restore control of terminal to shell
    tcsetpgrp(STDIN_FILENO, getpgrp());
}

// ---------- main loop ----------
int main() {
    install_signal_handlers();
    load_history();

    // Put shell in its own process group and take terminal
    pid_t shell_pgid = getpid();
    setpgid(shell_pgid, shell_pgid);
    tcsetpgrp(STDIN_FILENO, shell_pgid);

    char line_in[MAX_INPUT];

    while (1) {
        // prompt: user@host:cwd$
        char cwd[1024]; getcwd(cwd, sizeof(cwd));
        printf("mysh:%s$ ", cwd ? cwd : "");
        fflush(stdout);

        if (!fgets(line_in, sizeof(line_in), stdin)) {
            putchar('\n'); break;
        }
        line_in[strcspn(line_in, "\n")] = 0;
        char full_line[MAX_INPUT]; strncpy(full_line, line_in, sizeof(full_line)-1); full_line[sizeof(full_line)-1]=0;

        trim(line_in);
        if (!*line_in) continue;

        // history add (skip duplicate consecutive)
        if (history_len==0 || strcmp(history[history_len-1], full_line)!=0) add_history(full_line);

        // background?
        bool background = false;
        int len = (int)strlen(line_in);
        if (len>0 && line_in[len-1] == '&') {
            background = true;
            line_in[len-1] = 0;
            trim(line_in);
        }

        // Execute (builtins handled inside execute_line for single, else via run_builtin)
        execute_line(line_in, background, full_line);

        // cleanup finished jobs spam
        remove_done_jobs();
    }

    save_history();
    // free history
    for (int i=0;i<history_len;i++) free(history[i]);
    return 0;
}
