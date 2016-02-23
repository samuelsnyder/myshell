/*****************************
myshell.c
Samuel Snyder
samuel.l.snyder@gmail.com
*****************************/

#include <sys/wait.h> // WHNOHANG
#include <fcntl.h> // O_NONBLOCK
#include <sys/types.h> // waitpid()
#include <sys/wait.h>  // waitpid()
#include <stdio.h>
#include <unistd.h> 
#include <stdlib.h>
#include <string.h>
#include <errno.h> //ECHILD
#include <signal.h> 

#define MAX_INPUT 2048
#define MAX_ARGS 512
#define BACKGROUND 1
#define FOREGROUND 0

/* 	This struct is for the creation of 
	a linked list of background processes.
	For each pass through the main loop, this list
	will be checked for completed projects to remove and report
	When exiting, this list will be used to find projects to kill.
*/
struct process 
{
	int pid;
	int status;
	process *next; // next pid in list
	process *prev; // prev pid in list
};

/*
	This struct is for organizing the parts of a command (args, input for 
	redirection, output for redirection, whether it is to run in the background)
*/
struct command
{
	char* program; // FILE* program;
	char** args;
	int argn;
	char* input_file_name;
	char* output_file_name;
	int background;
};

int status; // fr storing status of last exiting foreground child process 
struct process * bg_pid_list = NULL; // storage for pids of bg processes

/*
	catches kill and interrupt, 2 and 15. It prints a message and sets status 
	to the signal, so that the status builtin will recognize that the most recent
	process closed due to a signal.
*/
void sigHandler(int sig);

/*
	Prints exit status message corresponding to an integer status.
*/
void printStatus(int statusIn);

/*
	Fills struct at s_command with default values of 0 or NULL, and allocates memory
*/
void initializeCommand(struct command *s_command);

/*
	prints the contents of a command struct for the purpose of debugging
*/
void printCommand(struct command * s_command);

/*
	Parses the string input according to the syntax:
		program [args...] [< input] [> ouput] [&]
	and fills command struct with corresponding values.
	If blank or begins with '#', it will return null pointer
*/
struct command* parseInput(char* input, struct command * s_command);

/*
	inspects the contents of a command to see if it calls a builtin
	task: cd, status, echo, or exit.
	If so, that task is carried out.
	cd: change directory. no arg is HOME
	status: prints exit status of last forground process to stdout
	echo: prints args to stdout
	exit: kills all background processes and exits

*/
int builtin(struct command* s_command);

int main(int argc, char const *argv[])
{
	char input[MAX_INPUT];  //  string buffer
	int pipe_fd[2]; // for getting output from background
	struct command *s_command = (struct command *) malloc(sizeof(struct command)); // will contained organized command from stdin

	// registering signal handler
	signal(SIGTERM, sigHandler);
	signal(SIGINT, sigHandler);

	// this pipe will be used to get the output of any background processes 
	if (pipe2(pipe_fd, O_NONBLOCK) == -1) {
  		perror("pipe");
  		exit(1);
	}

	// get bg_process list ready
	// this linked list contain pid's of backrgound processes so that they can
	// be checked for exit status
	bg_pid_list = (struct process *) malloc(sizeof (struct process));
	bg_pid_list->prev = NULL;
	bg_pid_list->next=NULL;

	while(1)
	{

		// Need to check if any done bg processes
		// 1st create a variable for tracking position thru list
		struct process *current = bg_pid_list;
		while(current->next != NULL){
			// 1st node is head, so move to next
			current = current->next;
			if (current->pid && waitpid(current->pid, &current->status, WNOHANG)){
				// this background process has finished
				// report this
				printf("Background pid %d is done: ", current->pid); printStatus(current->status);
				//remove this link from list
				current->prev->next = current->next;
			}
		}

		// Check pipe exit and see if there is anything for us to print
		// zero out buffer. Anything read will now be null terminated	
		bzero(input, MAX_INPUT);
		if ( read( pipe_fd[0], input, MAX_INPUT) > 0) {
			// pipe is not empty and is not blocked
			// print result
			printf("%s\n", input);
		}		

		// print prompt
		fflush(stdout);
		printf(":");

		// get input from stdin
		// this is the command
		if (! fgets(input ,MAX_INPUT, stdin)){
			printf("Error reading input.\n");
			return(1);
		}

		// parse input
		// the args, input, output, and background status will be stored in the struct s_command for convenience
		initializeCommand(s_command); // set to default values
		if(NULL == parseInput(input, s_command) ){ // put args into command struct
			// this is an unreadable commnad, either blank or comment, go to top of loop
			continue;
		}

		// builtin checks to see if command is a builtin, which it will then perform and return 0
		if (builtin(s_command) !=0 )
		{
				// not a builtin command, we will need to call exec to perform this command
				pid_t pid, wait_pid; //  pid after fork(), result of waitpid()

				// need to fork process so this process doesn't get clobbered
				// forking current process
				pid = fork();
				if(pid == 0){

					// if pid is 0, this is the child process

					// checking if if we are running in background, in order to redirect and annouce pid
					if (s_command->background == BACKGROUND)
					{	
						
						// setting stdin to be /dev/null
						// this way background process wont "steal" input
						freopen ("/dev/null","r",stdin);	

						// set stdout o be our pipe, this way the message won't interrupt
						// the foreground process
						// close the read side of the pipe becuase child won't read	
						close(pipe_fd[0]);

						// redirect stdout using dup2 into the pipe so that we can read it from the parent
						// after it has been written
        				dup2(pipe_fd[1], STDOUT_FILENO);
        				dup2(pipe_fd[1], STDERR_FILENO);
					}

					/* We need to redirect streams if input or output were specified.
					we waited to do that in the forked thread so that main thread still 
					has normal input/output.
					ref: http://www.cplusplus.com/reference/cstdio/freopen/
					*/
					// redirect input
					if (s_command->input_file_name != NULL)
					{
						// open with read permission because don't need to change file
						// and redirect standard in to file
						if (freopen (s_command->input_file_name,"r",stdin) == NULL){
							// couldn't open file
							perror("smallsh");
							exit(EXIT_FAILURE);
						} 
					}

					// redirect output
					if (s_command->output_file_name != NULL)
					{
						// open with write permission in order to output to file
						// and redirect standard in to file
						if ( freopen (s_command->output_file_name,"w",stdout) == NULL){
							perror("smallsh");
							exit(EXIT_FAILURE);
						}
					}

					// execute in this process
					/* Using this version of exec in order to use existing PATH
					and other environemt variables, and to supply arguments as 
					an array
					*/
					// https://en.wikipedia.org/wiki/Exec_(computing)
					if (execvp(s_command->program, s_command->args) == -1){
						// execution failed, probably a call that doesn't exist
						perror("smallsh");
					}
					// shouldn't be here unless there was a problem
					exit(EXIT_FAILURE);
				}
				else if (pid < 0) {
					// if pid < 0, something went wrong
					perror("smallsh");
				}
				else 
				{

					// we are in parent process, checking if child was background
					if (s_command->background != BACKGROUND)
					{
						// this was a foreground child, we will wait for 
						// child process to finsih and get status
						// http://linux.die.net/man/2/waitpid

						do {
							wait_pid = waitpid(pid, &status, WUNTRACED );
							// check to see if we have an errorish result, but that
							// it's not becuase the child closed before we could check
							if (wait_pid < 0 && errno != ECHILD){
								// error
								perror("waitpid");
								exit(EXIT_FAILURE);
							}
						}					
						while (!WIFEXITED(status) && !WIFSIGNALED(status)); // wait until exit or signal
					}
					else {

						// in foreground after background launch. announce process pid
						printf("background pid is %d\n", pid);

						// add it to our list of bg pids. make a cursor
						struct process *bg_process = bg_pid_list;
						
						// navigate to end of list
						while (bg_process->next != NULL) {
							bg_process = bg_process->next;
						}

						// add background process to list		
						bg_process->next = (struct process *) malloc(sizeof (struct process));
						bg_process->next->pid = pid;
						bg_process->next->prev = bg_process;
						bg_process->next->next = NULL;
					}
				}
			}
		}
	return 0;
}

void sigHandler(int sig){
	printf("Process %d received signal: %d\n",getpid(), sig );
	status = sig; // caught signals are interrupt and kill, 2, and 15
}

void printStatus(int statusIn){
	if (statusIn == SIGINT || statusIn == SIGTERM){
		printf("received signal: %d\n", statusIn );
	}
	else if (statusIn == 0 )
		printf("exit value %d\n", statusIn);
	else
		printf("exit value %d\n", 1);
}

void initializeCommand(struct command *s_command){
	int i;
	s_command->program = NULL; 
	s_command->argn = 0 ;
	s_command->args = (char **) malloc(MAX_ARGS * sizeof(char *));
	for (i = 0; i < MAX_ARGS; ++i)
	{
		s_command->args[i]=NULL;
	}
	s_command->input_file_name = NULL;
	s_command->output_file_name= NULL;
	s_command->background=FOREGROUND;
}

void printCommand(struct command * s_command){
	int i;
	printf("Command:\n");
	printf("\ts_command->program = %s\n", s_command->program  ); 
	printf("\ts_command->argn = %d\n",  s_command->argn) ;
	for (i = 0; i < s_command->argn; ++i)
	{
		printf("\t\ts_command[%d]=%s\n",i, s_command->args[i] );	
	}
	printf("\ts_command->input_file_name =%s\n", s_command->input_file_name);
	printf("\ts_command->output_file_name=%s\n", s_command->output_file_name);
	printf("\ts_command->background=%d\n", s_command->background);	
}

struct command* parseInput(char* input, struct command * s_command){

	char* tokens[MAX_ARGS + 5];
	int i;
	int tokensLength;
	const char* input_redir = "<";
	const char* output_redir = ">";
	const char* background_symb = "&";

	// get first token, check if empty
	if (!(tokens[0] = strtok(input, " \n")) ){
		// nothing there, return null pointer
		return NULL;
	}

	// if first token is #, this is a comment
	if (strcmp(tokens[0], "#") == 0 )
	{
		return NULL;
	}
	// collect the rest of the tokens
	i=1; tokensLength=1;
	while (tokens[i] = strtok(NULL, " \n") ){
		i++; tokensLength++;
	}
	
	// get program
	s_command->program= (char *) malloc( (strlen(tokens[0]) + 1) * sizeof (char));
	strcpy(s_command->program, tokens[0]);


	// get args from tokens
	// first arg will be name of program
	s_command->args[0]=(char *) malloc( sizeof(char) * (strlen(tokens[0]) + 1) );
	strcpy(s_command->args[0], s_command->program);
	s_command->argn = 1; //got our first arg

	// bail if only 1 token
	if (tokensLength == 1)
	{
		return s_command;
	}

	// get rest of args
	for (i = 1; i < tokensLength; ++i)
	{
		// check for special symbol then bail from loop
		if (strcmp(tokens[i], input_redir) == 0 
			|| strcmp(tokens[i], output_redir) == 0 
			|| strcmp(tokens[i], background_symb ) ==0)
		{
			break;
		}
		// copy token to arg
		s_command->args[i]=(char *) malloc( sizeof (char ) * (strlen(tokens[i])+1) );
		strcpy(s_command->args[i], tokens[i]);
		s_command->argn = i+1;
	}

	// bail if already was at last token
	if (i >= tokensLength){return s_command;}

	// get input file
	if (strcmp(tokens[i], input_redir) == 0)
	{
		i++; // go to next token
		// this is the file name of this commands input
		s_command->input_file_name = (char *) malloc( sizeof(char) * (1 +strlen(tokens[i]))) ;
		strcpy(s_command->input_file_name, tokens[i]);
		i++; // go to next token
	}

	// bail if already was at last token
	if (i >= tokensLength){return s_command;}

	// get output file
	if (strcmp(tokens[i], output_redir) == 0)
	{
		i++; // go to next token
		// this token is thefile name for output
		s_command->output_file_name = (char *) malloc( sizeof(char) * (1 +strlen(tokens[i]))) ;
		strcpy(s_command->output_file_name, tokens[i]);
		i++; // go to next token
	}

	// bail if already was at last token
	if (i >= tokensLength){return s_command;}

	// get background t/f
	if (strcmp(tokens[i], background_symb) == 0)
	{
		// run this in background
		s_command->background = BACKGROUND;
	}
	return s_command;
}

int builtin(struct command* s_command){
	int i;

	// echo
	if (strcmp(s_command->program, "echo") == 0)
	{
		// first arg is echo so skip
		for (i = 1; i < s_command->argn; ++i)
		{
			if (i > 1)
			{
				// print a space after first word
				printf(" ");
			}
			printf("%s", s_command->args[i]);
		}
		printf("\n");
		return 0;
	}

	// status
	if (strcmp(s_command->program, "status") == 0)
	{
		printStatus(status);
		return 0;
	}

	//cd
	if (strcmp(s_command->program, "cd") == 0)
	{
		if (s_command->argn == 1){
			// only 1 command, it is cd
			// change directory to home
        	if (chdir(getenv("HOME")) != 0){
    			perror("smallsh");
    		}
    	}
    	else
    	{
        	if (chdir(s_command->args[1]) != 0){
    			perror("smallsh");
    		}
    	}
    	return 0;
    }
    
	// exit
	if (strcmp(s_command->program, "exit") == 0)
	{
		free(s_command); 	// free command struct

		// kill any remaining background processes stored on llinked list
		struct process * current = bg_pid_list->next;
		while( current != NULL){
			if (current->pid != 0);
				if (kill(current->pid, SIGKILL) != 0){
					perror("smallsh");
				}
			current = current->next; // next link
			free(current->prev); // free memory from previous link
		}
		exit(EXIT_SUCCESS);
	}

	// not a builtin
	return -1;
}
