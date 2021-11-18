
// gcc -O3 -D_POSIX_C_SOURCE=200809 -Wall -std=c11 shell.c myshell.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
// #include <sys/syscall.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

/* changes the action for SIGINT (Cntr+C) to ignore instead of terminate  
 * output: 1 when error, 0 on success (from 2.1 and  "Error handling 2" in assignment)
 */
int prepare(void){
    if (signal(SIGINT, SIG_IGN) == SIG_ERR) {
        perror("changing signal failed");
        return 1;
    }
    return 0;
}

/* process_two is called on when "|" appears in command line, called from child.
 * input: arglist (cmd line in string array form), symbol_index ("|" index in arglist)
 * behavior: create two children, invoke at the same time
 * process1's output becomes input for process2
 * reference: https://www.youtube.com/watch?v=6xbLgZpOBi8
 * output: 0 if error, 1 on success (like process_arglist)
 * exit(1) if error occurs in child - exit(1) (from "Error handling 4" in assignment)
 */ 
int process_two(char** arglist, int symbol_index){
    int pipefds[2];
    if (pipe(pipefds) == -1) {
        perror("pipe failed");
        return 0;
    }
    int readerfd = pipefds[0];
    int writerfd = pipefds[1];

    int pid_one = fork();
    if (pid_one == -1) {
        perror("fork failed");
        return 0;
    } if (pid_one == 0) {
	    // child1, will be process1
        if (signal(SIGINT, SIG_DFL) == SIG_ERR) { // change SIGINT action back to default
            perror("changing signal failed");
            exit(1);
        }
        close(readerfd);
        if (dup2(writerfd,1) == -1) { // pipe becomes the output for process1
            perror("dup2 failed");
	     close(writerfd);
            exit(1);
        }
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
        if (signal(SIGINT, SIG_DFL) == SIG_ERR) { // change SIGINT action back to default
            perror("changing signal failed");
            exit(1);
        }
        close(writerfd);
        while(errno == EINTR){}; // process1 hasn't done enough
        if (dup2(readerfd,0) == -1) { // pipe becomes input for process2
            perror("dup2 failed");
            close(readerfd);
            exit(1);
        }
        char** arglist2 = arglist + symbol_index + 1; // "|" is at index i, process2 starts at i+1
        if (execvp(arglist2[0],arglist2) == -1) { // child2 becomes process2
            perror("execvp failed");
            exit(1);
        }
    }
    close(readerfd);
    close(writerfd);
    int wait_one= waitpid(pid_one,NULL,0);
    if (wait_one == -1 &&  errno != ECHILD && errno != EINTR){
	perror("waiting failed");
         return 0;
    }
    int wait_two= waitpid(pid_two,NULL,0);
    if (wait_two == -1 && errno != ECHILD && errno != EINTR ) {
            	perror("waiting failed");
            	return 0;
    }
    return 1;
}

/*  actual_processing is called on for all command types except for pipe commands
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
    } else if (pid == 0) { 
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
            if (signal(SIGINT, SIG_DFL) == SIG_ERR) { // set SIGINT action back to default
                perror("changing signal failed");
                exit(1);
            }
        } if (execvp(arglist[0],arglist) == -1) { // child becomes the process
            perror("execvp failed");
            exit(1);
        }
    }
    // Father process
    else{ 
	if (cmd_type == 0 || cmd_type == 4){ // not pipe or &
        	int w = waitpid(pid,NULL, WUNTRACED);
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
