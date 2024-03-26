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
int cmd_pwd(struct tokens* tokens);
int cmd_cd(struct tokens* tokens);
void execute(struct tokens* tokens, char*);
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
    {cmd_exit, "exit", "exit the command shell"},
    {cmd_pwd, "pwd", "show the current working directory"},
    {cmd_cd, "cd", "takes one argument and changes the current working directory to that directory"},
};

int cmd_cd(struct tokens* token) {
  int ret = chdir(token->tokens[1]);
  if(ret == -1) {
    printf("cmd_cd: failed to change working directory\n");
    exit(-1);
  }
  return 0;
}

int cmd_pwd(unused struct tokens* tokens) {
  char *path = getcwd(NULL, 0);
  if(path == NULL) {
    printf("cmd_pwd: failed to allocate memory for pwd\n");
    exit(-1);
  }
  fprintf(stdout, "%s\n",path);
  free(path);
  return 0;
}
/* Prints a helpful description for the given command */
int cmd_help(unused struct tokens* tokens) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    printf("%s - %s\n", cmd_table[i].cmd, cmd_table[i].doc);
  return 1;
}

/* Exits this shell */
int cmd_exit(unused struct tokens* tokens) { exit(0); }

/* Looks up the built-in command, if it exists. */
int lookup(char cmd[]) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    if (cmd && (strcmp(cmd_table[i].cmd, cmd) == 0))
      return i;
  return -1;
}
//一个一个试
char* get_full_path(char* short_name, char* char_env) {
  FILE *file = fopen(short_name, "r");
  if(file != NULL) {
    fclose(file);
    return short_name;
  }
  //fprintf(stdout, "%s\n", char_env);
  char *full_path = (char*)malloc(4096);
  if(full_path == NULL) {
    fprintf(stdout, "Malloc Error\n");
    return NULL;
  }
  char *saveptr;
  char* token = strtok_r(char_env, ":", &saveptr);

  while (1) {
    if(token == NULL) {
      break;
    }
    int len = strlen(token);
    memcpy(full_path, token, len + 1);
    full_path[len] = '/';
    full_path[len + 1] = '\0';
    strcat(full_path, short_name);

    file = fopen(full_path, "r");
    if(file != NULL) {
      fclose(file);
      return full_path;
    }

    token = strtok_r(NULL, ":", &saveptr);
  }
  free(full_path);
  return NULL;
}

int child_exec(char* full_path, char** argv, int input_file, int output_file){
  int status;
  int pid = fork();
  if(pid < 0){
    fprintf(stdout, "fork error\n");
    return -1;
  }

  if(pid == 0) {
    if(input_file != 0) { //不是stdin
      dup2(input_file, STDIN_FILENO);
    }

    if(output_file != 1) { //不是stdout
      dup2(output_file, STDOUT_FILENO);
    }
    if(execv(full_path, argv) == -1) {  
      free(full_path);
      fprintf(stdout, "There is an error when executing\n");
      exit(-1);
    }
    free(full_path);
    exit(0);
  } else {
    wait(&status);
    return status;
  }
}


void execute(struct tokens* tokens, char* char_env) {
  char *short_name = tokens_get_token(tokens, 0);
  if(short_name == NULL) {
    fprintf(stdout, "The path is NULL\n");
    return;
  }
  char *path = get_full_path(short_name, char_env);
  if(path == NULL) {
    fprintf(stdout, "Could not get full path\n");
    return;
  }

  int input_file = 0;
  int output_file = 1;
  int pipes[2];//0->read 1 ->write

  int len = tokens_get_length(tokens);
  int argc = 0;
  char* argv[len + 1]; //最后一个元素必须是NULL
  for(int i = 0; i < len; i++) {
    char* args = tokens_get_token(tokens, i);
    if(strcmp(args, "<") == 0) {
      ++i;
      args = tokens_get_token(tokens, i);
      input_file = open(args, O_RDONLY);
    } else if(strcmp(args, ">") == 0) {
      ++i;
      args = tokens_get_token(tokens, i);
      output_file = creat(args,S_IRUSR | S_IRWXU);
    } else if(strcmp(args, "|") == 0){ // pa | pb | pc 
      argv[argc] = NULL; //pa pb or pc
      pipe(pipes);
      output_file = pipes[1];
      child_exec(path, argv, input_file, output_file);
      close(output_file);
      output_file = 1;
      argc = 0;
      if(input_file != STDIN_FILENO) {
        close(input_file);
      }
      input_file = pipes[0];
    }else {
        // argv[i] = malloc(strlen(args));
        // memcpy(argv[i], args, strlen(args));
        argv[argc] = args;
        argc++;
    }

  }
  argv[argc] = NULL;

  // int i = 0;
  // while(argv[i] != NULL) {
  //   fprintf(stdout, "%s\n", argv[i++]);
  // }
  child_exec(path, argv, input_file, output_file);
  //free(argv);
  // for(int i = 0; i < argc; i++) {
  //   free(argv[i]);
  // }

  return;
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
  char *char_env = getenv("PATH");
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
      /* REPLACE this to run commands as programs. */
      /*第二项任务修改这里执行命令*/
      char *c_env = malloc(strlen(char_env) + 1);
      memcpy(c_env, char_env, strlen(char_env)+1);
      execute(tokens, c_env);
      free(c_env);
    }

    if (shell_is_interactive)
      /* Please only print shell prompts when standard input is not a tty */
      fprintf(stdout, "%d: ", ++line_num);
    /* Clean up memory */
    tokens_destroy(tokens);
  }

  free(char_env);
  return 0;
}
