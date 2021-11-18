// gcc -O3 -D_POSIX_C_SOURCE=200809 -Wall -std=c11 shell.c myshell.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

/* if a child finished before the parent- a zombie is created.
 * if the parent does not wait for child- the zombie is never erased.
 * this new handler for SIGCHLD "waits" for the finished child and therefore prevents zombies
 * reference: https://docs.oracle.com/cd/E19455-01/806-4750/signals-7/index.html
 */
void sigchld_handler(){
    int errno_1 = errno;
    while(waitpid(-1, 0, WNOHANG)>0){}
    // if (waitpid(-1, 0, WNOHANG) == -1 && errno != ECHILD && errno != EINTR) {
    //    perror("waiting failed");
    //    exit(1);
    // }
    errno = errno_1;
}


/* behavior: changes the action for SIGINT (Cntr+C) to ignore instead of terminate, changes the handler for SIGCLD to prevent zombies
 * output: 1 when error, 0 on success (from 2.1 and  "Error handling 2" in assignment) */
int prepare(void){
    // Change action for SIGINT
    if (signal(SIGINT, SIG_IGN) == SIG_ERR) {
        perror("changing signal failed");
        return 1;
    }
    // Change action for SIGCLD
    struct sigaction new_action;
    memset(&new_action, 0, sizeof(new_action));
    new_action.sa_handler = sigchld_handler;
    new_action.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCLD,&new_action,0) == -1) {
        perror("changing signal failed");
        return 1;
    }
    return 0;
}

/* behavior: changes action for SIGINT back to default
* is called from children that are not in & commmands */
void SIGINT_action_to_SIGDFL(){
    if (signal(SIGINT, SIG_DFL) == SIG_ERR) { // change SIGINT action back to default
        perror("changing signal failed");
        exit(1);
    }
}

/* process_two is called on when "|" appears in command line
 * input: arglist (cmd line in string array form), symbol_index ("|" index in arglist)
 * behavior: create two children, invoke at the same time, process1's output becomes input for process2
 * reference: https://www.youtube.com/watch?v=6xbLgZpOBi8
 * output: 0 if error, 1 on success (like process_arglist)
 * exit(1) if error occurs in child - exit(1) (from "Error handling 4" in assignment) */
int process_two(char** arglist, int symbol_index){
    /* Create pipe */
    int pipefds[2];
    if (pipe(pipefds) == -1) {
        perror("pipe failed");
        return 0;
    }
    int readerfd = pipefds[0];
    int writerfd = pipefds[1];

    /* Invoke both processes */
    int pid_one = fork();
    if (pid_one == -1) {
        perror("fork failed");
        return 0;
    }
    if (pid_one == 0) {
        // child1, will be process1
        SIGINT_action_to_SIGDFL();
        close(readerfd);
        if (dup2(writerfd,1) == -1) { // pipe becomes the output for process1
            perror("dup2 failed");
            close(writerfd);
            exit(1);
        }
        close(writerfd);
        if (execvp(arglist[0],arglist) == -1) { // child1 becomes process1
            perror("execvp failed");
            exit(1);
        }
    }
    int pid_two = fork();
    if (pid_two == -1){
        perror("fork failed");
        return 0;
    }
    if (pid_two == 0){
        // child2, will be process 2
        SIGINT_action_to_SIGDFL();
        close(writerfd);
        if (dup2(readerfd,0) == -1) { // pipe becomes input for process2
            close(readerfd);
            perror("dup2 failed");
            exit(1);
        }
        close(readerfd);
        char** arglist2 = arglist + symbol_index + 1; // "|" is at index i, process2 starts at i+1
        while(errno == EINTR){}; // wait until process1 has done enough
        if (execvp(arglist2[0],arglist2) == -1) { // child2 becomes process2
            perror("execvp failed");
            exit(1);
        }
    }
    close(readerfd);
    close(writerfd);
    int wait_one = waitpid(pid_one,NULL,0);
    if (wait_one == -1 && errno != ECHILD && errno != EINTR){
        perror("waiting failed");
        return 0;
    }
    int wait_two= waitpid(pid_two,NULL,0);
    if (wait_two == -1 && errno != ECHILD && errno != EINTR) {
        perror("waiting failed");
        return 0;
    }
    return 1;
}


/* actual_processing is called on for all command types except for pipe commands
 * input: arglist (cmd line in string array form), cmd_type (0-regular, 1-'&', 2-pipe, 3-'>'), symbol_index (&/>/| index in arglist)
 * behavior: invokes given command
 * output: 0 if error, 1 on success (like process_arglist)
 */
int actual_processing(char** arglist, int cmd_type, int symbol_index){
    if (cmd_type == 2){
        return process_two(arglist,symbol_index);
    }
    int pid = fork();
    if (pid == -1) {
        perror("fork failed");
        return 0;
    }
    if (pid == 0) {
        // Child process, exit(1) or errors (from "Error handling 4")
        if (cmd_type == 3) {
            char *output_file_name = arglist[symbol_index + 1]; // ">" at index i, filename from i+1
            // reference: https://www.cs.utexas.edu/~theksong/2020/243/Using-dup2-to-redirect-output/
            int output_file = open(output_file_name, O_CREAT | O_RDWR, 0600); // O_CREAT to create if doesn't exist
            if (dup2(output_file, 1) == -1) { // specified file becomes stdout
                close(output_file);
                perror("dup2 failed");
                exit(1);
            }
            close(output_file);
        } if (cmd_type != 1) { // not &
            SIGINT_action_to_SIGDFL();
        } if (execvp(arglist[0],arglist) == -1) { // child becomes the process
            perror("execvp failed");
            exit(1);
        }
    }
    if (pid > 0){
        // Father process
        if (cmd_type == 0 || cmd_type == 4){ // regular or >
            int w = waitpid(pid, NULL, WUNTRACED);
            if (w == -1 && errno != ECHILD && errno != EINTR) {
                perror("waiting failed");
                return 0;
            }
        }
    }
    return 1;
}

/* input: arglist (command line in a form or array of strings), count (num of words in the command line +1)
 * arglist[count] == NULL
 * arglist[count-1] is the last real element
 * behavior: marks cmd_type (0-regular, 1-'&', 2-pipe, 3-'>') and calls other functions to invoke command
 */
int process_arglist(int count, char** arglist){
    int cmd_type = 0;
    int symbol_index = 0;
    char* c;
    for (int i = count-1 ; i>=0 ; i--) {
        c = arglist[i];
        if (i == count-1 && strcmp(c,"&") == 0) {
            cmd_type = 1;
        }
        if (strcmp(c,"|") == 0) {
            cmd_type = 2;
        }
        if (strcmp(c,">") == 0) {
            cmd_type = 3;
        }
        if (cmd_type > 0) {
            symbol_index = i;
            arglist[symbol_index] = NULL;
            break;
        }
    }
    return actual_processing(arglist, cmd_type, symbol_index);
}

int finalize(void){
    return 0;
}
