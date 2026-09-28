#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "tokenizer.h"

/* Convenience macro to silence compiler warnings about unused function parameters. */
#define unused __attribute__((unused))

/* Whether the shell is connected to an actual terminal or not. */
bool shell_is_interactive;

/* File descriptor for the shell input */
int shell_terminal;

/* Terminal mode settings for the shell */
struct termios shell_tmodes;

/* Process group id for the shell */
pid_t shell_pgid;

int cmd_exit(struct tokens* tokens);
int cmd_help(struct tokens* tokens);

/* Built-in command functions take token array (see parse.h) and return int */
typedef int cmd_fun_t(struct tokens* tokens);

/* Built-in command struct and lookup table */
typedef struct fun_desc {
  cmd_fun_t* fun;
  char* cmd;
  char* doc;
} fun_desc_t;

fun_desc_t cmd_table[] = {
    {cmd_help, "?", "show this help menu"},
    {cmd_exit, "exit", "exit the command shell"}
};

/* Prints a helpful description for the given command */
int cmd_help(unused struct tokens* tokens) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    printf("%s - %s\n", cmd_table[i].cmd, cmd_table[i].doc);
  return 1;
}

/* Exits this shell */
int cmd_exit(unused struct tokens* tokens) { exit(0); }

char* resolve_path(char* cmd){
    /* If command already contains a path, use it directly. */
  if (strchr(cmd, '/') != NULL) {
    if (access(cmd, X_OK) == 0) {
      return strdup(cmd);
    }
    return NULL;
  }

  char* path_env = getenv("PATH");
  if (!path_env)return NULL;
  char* path_cpy = strdup(path_env);
  char* saveptr;
  char* dir = strtok_r(path_cpy, ":", &saveptr);
  char full_path[1024];

  while(dir != NULL){
    snprintf(full_path, sizeof(full_path), "%s/%s", dir, cmd);
    if (access(full_path, X_OK) == 0){
      free(path_cpy);
      return strdup(full_path);
    }
    dir = strtok_r(NULL, ":", &saveptr);
  }
  free(path_cpy);
  return NULL;
}



void run_program(struct tokens* tokens){
  size_t num_tokens = tokens_get_length(tokens);
  int len_pipe_arr = 0;

  /* malloc cmds, cmds[len_pipe_arr+1][] */
  for (size_t i = 0; i < num_tokens; i++){
    char* token = tokens_get_token(tokens, i);
    if(strcmp(token, "|") == 0){
      len_pipe_arr++; 
    } 
  } 
  int num_childp = len_pipe_arr +1;
  char*** cmds = malloc((num_childp+1) * sizeof(char**)); 
  if (cmds == NULL){
    free(cmds);
    perror("malloc cmds failed");
    exit(1);
  }
  cmds[num_childp] = NULL;

  /* malloc argv, cmds[][argv] */
  char** argv = malloc((num_tokens +1) * sizeof(char*));  
  char** input_files = calloc(num_childp, sizeof(char*));
  char** output_files = calloc(num_childp, sizeof(char*));
  size_t argc = 0;
  int count_len_pipe_arr = 0;
  for (size_t i = 0; i < num_tokens; i++){
    /*check '<' and '>' for each token */
    char* token = tokens_get_token(tokens, i);
    if (strcmp(token, "<") == 0){
      if (i+1 < num_tokens){
        input_files[count_len_pipe_arr] = tokens_get_token(tokens, i+1);
        i++;
      }
    }else if(strcmp(token, ">") == 0){
      if (i+1 < num_tokens){
        output_files[count_len_pipe_arr] = tokens_get_token(tokens, i+1);
        i++; 
      }
    }else if(strcmp(token, "|") == 0){
      if (i+1 < num_tokens){
        argv[argc] = NULL;
        argc = 0;
        cmds[count_len_pipe_arr] =  argv;
        argv = malloc((num_tokens +1) * sizeof(char*));   
        if (argv == NULL){
          for (int j = 0; j < count_len_pipe_arr; j++){
            free(cmds[j]);
            free(cmds);
            perror(" malloc argv failed");
            exit(1);
          }
        } 
        count_len_pipe_arr++;
      }
    }else{
      argv[argc] = token;
      argc++;      
    }    
  }
  argv[argc] = NULL;
  cmds[num_childp-1] = argv;

  /* create pipe*/
  int pipe_arr[len_pipe_arr][2];
  for (int i = 0; i < len_pipe_arr; i++){
    if (pipe(pipe_arr[i]) < 0){
      perror("pipe failed");
      exit(1);
    }
  }
  /* get shell process group*/
  pid_t shellpgrp = getpgrp();

  /* ignore signal from keyboarb shortcut*/
  signal(SIGINT, SIG_IGN);
  signal(SIGTSTP, SIG_IGN);
  signal(SIGTTIN, SIG_IGN);
  signal(SIGTTOU, SIG_IGN);

  /* create child process*/ 
  for (int i = 0; i < num_childp; i++){
    pid_t pid = fork();  

    /* child process */
    if (pid == 0){
      pid_t cpgrp = getpgrp();

      if (i == 0){
        setpgrp();
      }else{
        setpgid(0, cpgrp);
      }
      tcsetpgrp(0, cpgrp);

      signal(SIGINT, SIG_DFL);   // Ctrl+C
      signal(SIGQUIT, SIG_DFL);  /*  Ctrl+\ */
      signal(SIGTSTP, SIG_DFL);  // Ctrl+Z
      signal(SIGTTIN, SIG_DFL);  // Background read from tty
      signal(SIGTTOU, SIG_DFL);  // Background write to tty

      char* cmd= cmds[i][0];
      /* path resolution*/
      char* path = resolve_path(cmd);
      if (path == NULL) {
        fprintf(stderr, "%s: command not found\n", cmd);
        free(path);
        for (int j = 0; j < len_pipe_arr; j++){
          free(cmds[j]);
        }
        free(cmds);
        exit(1);
      }

      /* first child process no need changes its STDIN*/
      if (i > 0){
        dup2(pipe_arr[i-1][0], STDIN_FILENO);        
      }

      /* last child process no need changes its STDOUT*/
      if (i < len_pipe_arr){
        dup2(pipe_arr[i][1], STDOUT_FILENO);      
      }

      /* close pipe*/
      for (int j = 0; j < len_pipe_arr; j++){
        close(pipe_arr[j][0]);
        close(pipe_arr[j][1]);
      }

      /* redirection */
      /* check "<" */
      if (input_files[i] != NULL){
        int in_fd = open(input_files[i], O_RDONLY);
        if (in_fd < 0){
          perror("open input file failed");
          exit(1);
        }
        dup2(in_fd, STDIN_FILENO);
        close(in_fd);
      }

      /* check ">" */
      if(output_files[i] != NULL){
        int out_fd = open(output_files[i], O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out_fd < 0){
          perror("open output file failed");
          exit(1);
        }
        dup2(out_fd, STDOUT_FILENO);
        close(out_fd);
      }
      /* program execution */
      execv(path, cmds[i]);

      perror("execv failed");
      free(path);
      exit(1);
      }else if(pid < 0){
        perror("fork failed");
        for (int j = 0; j < len_pipe_arr; j++){
          free(cmds[j]);
        }
        free(cmds);
        return;
      }

      for (int j = 0; j < len_pipe_arr; j++){
        close(pipe_arr[j][0]);
        close(pipe_arr[j][1]);
      }
      
      for (int j = 0; j < num_childp; j++){
        int status;
        wait(&status);
      }

      /* push the pidgrp to the foreground to accept new terminal input*/
      tcsetpgrp(0, shellpgrp);

      for (int j = 0; j < num_childp; j++){
        free(cmds[j]);
      }
      free(cmds);
      free(input_files);
      free(output_files);      
  }
  }

/* Looks up the built-in command, if it exists. */
int lookup(char cmd[]) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    if (cmd && (strcmp(cmd_table[i].cmd, cmd) == 0))
      return i;
  return -1;
}

/* Intialization procedures for this shell */
void init_shell() {
  /* Our shell is connected to standard input. */
  shell_terminal = STDIN_FILENO;

  /* Check if we are running interactively */
  shell_is_interactive = isatty(shell_terminal);

  if (shell_is_interactive) {
    /* If the shell is not currently in the foreground, we must pause the shell until it becomes a
     * foreground process. We use SIGTTIN to pause the shell. When the shell gets moved to the
     * foreground, we'll receive a SIGCONT. */
    while (tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp()))
      kill(-shell_pgid, SIGTTIN);

    /* Saves the shell's process id */
    shell_pgid = getpid();

    /* Take control of the terminal */
    tcsetpgrp(shell_terminal, shell_pgid);

    /* Save the current termios to a variable, so it can be restored later. */
    tcgetattr(shell_terminal, &shell_tmodes);
  }
}

int main(unused int argc, unused char* argv[]) {
  init_shell();

  static char line[4096];
  int line_num = 0;

  /* Please only print shell prompts when standard input is not a tty */
  if (shell_is_interactive)
    fprintf(stdout, "%d: ", line_num);

  while (fgets(line, 4096, stdin)) {
    /* Split our line into words. */
    struct tokens* tokens = tokenize(line);

    /* Find which built-in function to run. */
    int fundex = lookup(tokens_get_token(tokens, 0));

    if (fundex >= 0) {
      cmd_table[fundex].fun(tokens);
    } else {
      run_program(tokens);
    }

    if (shell_is_interactive)
      /* Please only print shell prompts when standard input is not a tty */
      fprintf(stdout, "%d: ", ++line_num);

    /* Clean up memory */
    tokens_destroy(tokens);
  }

  return 0;
}
