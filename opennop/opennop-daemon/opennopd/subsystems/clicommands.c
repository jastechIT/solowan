/*

  clicommands.c

  This file is part of OpenNOP-SoloWAN distribution.
  It is a modified version of the file originally distributed inside OpenNOP.

  Original code Copyright (C) 2014 OpenNOP.org (yaplej@opennop.org)

  Modifications Copyright (C) 2014 Center for Open Middleware (COM)
                                   Universidad Politecnica de Madrid, SPAIN

  Modifications description: <mod_description>

    OpenNOP is an open source Linux based network accelerator designed 
    to optimise network traffic over point-to-point, partially-meshed and 
    full-meshed IP networks.

    OpenNOP-SoloWAN is an enhanced version of the Open Network Optimization
    Platform (OpenNOP) developed to add it deduplication capabilities using
    a modern dictionary based compression algorithm.

    SoloWAN is a project of the Center for Open Middleware (COM) of Universidad
    Politecnica de Madrid which aims to experiment with open-source based WAN
    optimization solutions.

  References:

    OpenNOP: http://www.opennop.org
    SoloWAN: solowan@centeropenmiddleware.com
             https://github.com/centeropenmiddleware/solowan/wiki
    Center for Open Middleware (COM): http://www.centeropenmiddleware.com
    Universidad Politecnica de Madrid (UPM): http://www.upm.es

  License:

    OpenNOP and OpenNOP-SoloWAN are free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    OpenNOP and OpenNOP-SoloWAN are distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Foobar.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stddef.h>
#include <pthread.h>

#include <sys/socket.h>

#include "clicommands.h"
#include "climanager.h"
#include "logger.h"

static struct command_head *head;
const char delimiters[] = " ";
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
//struct command *tail;

struct command* allocate_command(){
	struct command *newcommand = (struct command *) malloc (sizeof (struct command));
	if(newcommand == NULL){
		fprintf(stdout, "Could not allocate memory... \n");
		exit(1);
	}
	newcommand->next = NULL;
	newcommand->prev = NULL;
	newcommand->child.next = NULL;
	newcommand->child.prev = NULL;
	pthread_mutex_init(&newcommand->child.lock,NULL);
	newcommand->command_handler = NULL;
	newcommand->hasparams = false;
	return newcommand;
}

int detect_delim_count(char *i_str, const char *delim){
    char *itr_str = NULL;
    char *parser = NULL;
    int count = 0;

    itr_str = i_str;
    for (parser = strtok(itr_str, delim); parser && *parser ; parser = strtok(NULL, delim))
    {
	if (strcmp(parser, delim) != 0)
            count++;
    }

    return (count-1);
}


/*
 * This replaces cli_process_message().
 * It can execute multiple commands that share the same name.
 */
int execute_commands(int client_fd, const char *command_name, int d_len){
	char *token, *cp, *saved_token;
	int parametercount = 0, idx = 0;
	int shutdown = CLI_ERROR;
	char **parameters = NULL; //dynamic array of pointers to tokens that are parameters
	//char **tempparameters = NULL;
	//char *parameter = NULL;
	struct command_head *currentnode = NULL;
	struct command *currentcommand = NULL;
	struct command *executedcommand = NULL;
	char message[LOGSZ];

	sprintf(message, "CLI: Begin processing a command.\n");
	logger(LOG_INFO, message);

	if((head == NULL) || (head->next == NULL)){
		/*
		 * Cannot execute any commands if there are none.
		 */
		sprintf(message, "CLI: No known commands.\n");
		logger(LOG_INFO, message);
		shutdown = CLI_INV_CMD;
		return shutdown;
	}

	/*
	 * Make writable copy of *command_name for separating into GNU string TOKENS.
	 * The TOKENS are separated by spaces.
	 */
	cp = strdup(command_name);

	/*
	 * Initialize TOKEN.
	 * The delimiters are globally defined for consistency.
	 */
	token = strtok_r(cp, delimiters, &saved_token);
	currentnode = head;

	while((token != NULL) && (executedcommand == NULL)){
		/* Sharwan: We should check first, if the command is help,Help or ?
		*      Help in any case is good , but other commands are not
		*/
		if((!strcasecmp(token,"help")) || (!strcmp(token,"?"))){
			cli_node_help(client_fd, currentnode);
			break;
		}

		currentcommand = find_command(currentnode,token);
		if(currentcommand == NULL){
			/*
			 * We didn't find any commands.
			 * Show help for the current node then break.
			 * Need the help function to accept the currentnode as a parameter.
			 */
			sprintf(message, "CLI: Cound'nt find the command.\n");
			logger(LOG_INFO, message);
			/* If we're exiting from this point, free the allocations made  */
			if (cp){
				free(cp);
				cp = NULL;
			}
			shutdown = CLI_INV_CMD;
			break;
		} else {
			/* We've got some command or token */
			if(currentcommand->child.next != NULL){
				sprintf(message, "CLI: Command [%s] has children.\n", token);
				logger(LOG_INFO, message);
				currentnode = &currentcommand->child;
				currentcommand = NULL;
			}else{
				/*
				 * We found a command in this node.
				 * There are no other child nodes so its the last one.
				 */
				sprintf(message, "CLI: Locating [%s] command .\n", token);
				logger(LOG_INFO, message);

				while((executedcommand == NULL) && (currentcommand != NULL)){
					sprintf(message, "CLI: Current command [%s].\n", currentcommand->command );
					logger(LOG_INFO, message);

					if(currentcommand->command_handler == NULL){
						sprintf(message, "CLI: Command has no handler.\n");
						logger(LOG_INFO, message);
					}

					if((!strcmp(currentcommand->command,token)) && (currentcommand->command_handler != NULL)){

						if(currentcommand->hasparams == true){
							sprintf(message, "CLI: Command has parameters.\n");
							logger(LOG_INFO, message);
							/*
							 * NOTE: I found that you cannot pass *cp because its modified by strtok_r()
							 * The modification removed the " " delimiter and must replace it with a \0
							 * to terminate that TOKEN.  This prevents *cp from being re-parsed as a parameter.
							 * As a solution we can pass the original command_name and try to process it more then.
							 * Another solution might to be make another copy of command_name then remove the
							 * original command somehow to pass only the remaining arguments/parameters.
							 * This will require saving the whole original command in the command structure
							 * so it can be referenced again.
							 *
							 * UPDATE: I posted a question at LinuxQuestions about this.
							 * One very good idea is to create a dynamic array of the TOKENs and
							 * pass that to the function as the parameter.
							 * http://www.linuxquestions.org/questions/programming-9/parse-string-tokens-and-pass-remaining-as-parameter-4175468498/#post4984764
							 */

							if(token != NULL){
								if (strlen(saved_token) == 0) {
				                            	    sprintf(message, "CLI: Command has no parameters.\n");
				                            	    logger(LOG_INFO, message);
				                            	    shutdown = (currentcommand->command_handler)(client_fd, NULL, 0);
								}
								else {
								    char *temp_token = (char *) malloc (strlen(saved_token) + 1);
				                                    memcpy(temp_token, saved_token, strlen(saved_token));
								    temp_token[strlen(saved_token)] = '\0';
								    idx = detect_delim_count(temp_token, delimiters);
				                                    if (temp_token)
				                                        free(temp_token);
                                				    parameters = malloc((idx + 1 ) * sizeof (char *));
				                                    if ( NULL == parameters){
				                                        fprintf(stdout, "CLI: Couldn't allocate sufficient memory . Exiting \n");
		                               			        exit(1);
               					                     }
				                                    for( parametercount = 0 , token = strtok(saved_token, " "); parametercount <= idx ; ++parametercount , token = strtok(NULL, " ")){
				                                        parameters[parametercount] = calloc(30 , strlen(token) + 1);
				                                        if ( NULL == parameters[parametercount]){
				                                            fprintf(stdout,"CLI: Couldn't allocate sufficient memory. Exiting \n");
				                                            exit(1);
				                                        }
				                                        strncpy(*(parameters + parametercount), token, strlen(token));
				                                    }
                                                                    /* Execute the command finally */
				                                    shutdown = (currentcommand->command_handler)(client_fd, parameters, parametercount);
								}
							   } 
							} else {
				                            /*
				                             * We might want to verify no other TOKENs are left.
       					                     * If the command has no params none should have been given.
				                             */
				                            sprintf(message, "CLI: Command has no parameters.\n");
				                            logger(LOG_INFO, message);
				                            shutdown = (currentcommand->command_handler)(client_fd, NULL, 0);
                        				}
				                        executedcommand = currentcommand;

						}else{
                        				shutdown = CLI_ERR_CMD;
                        				sprintf(message, "CLI: Command did not match.\n");
                        				logger(LOG_INFO, message);

						}
						currentcommand = currentcommand->next;
					}
					/*
				 	 * Finished executing all commands in the node.
				 	*/
					if (parameters != NULL){
			                    for (parametercount = 0 ; parametercount <= idx; ++parametercount)
                        			if (parameters[parametercount])
                            			   free(parameters[parametercount]);
					    free(parameters); /* If we directly do this, we'll get bad SIGSEGV's */
					}
			}
		}

		token = strtok_r(NULL, delimiters, &saved_token); //Fetch the next TOKEN of the command.
		if (token == NULL && shutdown == CLI_ERROR) {
			cli_node_help(client_fd, currentnode);
		}
		
	}
	if (cp){
		free(cp);
		cp = NULL;
	}
	/*
	if(shutdown != 1){ // Don't show the last prompt if we are done.
		cli_prompt(client_fd);
	}
	*/
//	char msg[MAX_BUFFER_SIZE] = { 0 };
//	sprintf(msg, "ENDOFCOMMAND\n");
//	printf("client_fd: %d, mensaje: %sEOV\n", client_fd, msg);
//	cli_send_feedback(client_fd, msg);
	return shutdown;
}

/*
 * This needs to be re-written to store the commands in a tree.
 * NOTE: Calls to strtok() do not seem to be thread safe.
 * So calls to register_command() are globally protected
 * with a mutex lock.  This will be the same lock used to
 * parse parameters for command.  Unless someone helps me make
 * it thread safe.  Maybe strtok_r()?
 * UPDATE: I am trying to use strtok_r().  It seems to be working.
 */

int register_command(const char *command_name, t_commandfunction handler_function, bool hasparams, bool hidden){
	char *token, *cp, *saved_token;
	struct command_head *currentnode = NULL;
	struct command *currentcommand = NULL;
	char message[LOGSZ];

	pthread_mutex_lock(&lock);
	sprintf(message, "CLI: Begin registering [%s] command.\n", command_name);
	logger(LOG_INFO, message);

	if(head == NULL){
		head = (struct command_head *) malloc (sizeof (struct command_head));
		head->next = NULL;
		head->prev = NULL;
		pthread_mutex_init(&head->lock,NULL);
	}

	/*
	 * Make writable copy of *command_name for separating into GNU string TOKENS.
	 * The TOKENS are separated by spaces.
	 */
	cp = strdup(command_name);

	/*
	 * Initialize TOKEN.
	 * The delimiters are globally defined for consistency.
	 */
	token = strtok_r(cp, delimiters, &saved_token);
	currentnode = head;



	while(token != NULL){
		pthread_mutex_lock(&currentnode->lock); //Prevent race condition when saving command tree.
		sprintf(message, "CLI: Register [%s] token.\n",token);
		logger(LOG_INFO, message);
		/*
		 * Search the current node for a
		 * command matching the current TOKEN.
		 */
		currentcommand = find_command(currentnode,token);

		if(currentcommand != NULL){
			/*!
			 *Found the command for the current TOKEN.
			 *Set it as the current node and search it for the next TOKEN.
			 */
			sprintf(message, "CLI: Found an existing token.\n");
			logger(LOG_INFO, message);
		}else{
			/*
			 * Did not find the command for the current TOKEN
			 * We have to create it.
			 */
			sprintf(message, "CLI: Did not find an existing token.\n");
			logger(LOG_INFO, message);
			currentcommand = allocate_command();
			currentcommand->command = token;

			if(currentnode->next == NULL){
				sprintf(message, "CLI: Creating first command in node.\n");
				logger(LOG_INFO, message);
				currentnode->next = currentcommand;
				currentnode->prev = currentcommand;
			}else{
				sprintf(message, "CLI: Creating new command in node.\n");
				logger(LOG_INFO, message);
				currentnode->prev->next = currentcommand;
				currentcommand->prev = currentnode->prev;
				currentnode->prev = currentcommand;
			}
		}
		pthread_mutex_unlock(&currentnode->lock);
		currentnode = &currentcommand->child;
		token = strtok_r(NULL, delimiters, &saved_token); //Fetch the next TOKEN of the command.
	}

	//This was the last TOKEN of the command so assign the function here.
	if(currentcommand != NULL){
		currentcommand->command_handler = handler_function;
		currentcommand->hasparams = hasparams;
		currentcommand->hidden = hidden;
	}

	pthread_mutex_unlock(&lock);

	return 0;
}

/*
 * Called by  cli_process_message() in climanager.c.
 * const char *command_name = command the user typed.
 */
struct command* lookup_command(const char *command_name){
	struct command *currentcommand;
	currentcommand = head->next;

	while(currentcommand != NULL ){
		if (!strcmp(currentcommand->command, command_name))
			return currentcommand;
		currentcommand = currentcommand->next;
	}
	
	/* Sharwan Joram: If we are here, then we didn't find any command with us */
	return NULL;
}

/*
 */
struct command* find_command(struct command_head *currentnode, char *command_name){
	struct command *currentcommand;
	currentcommand = currentnode->next;

	while(currentcommand != NULL ){
		if (!strcmp(currentcommand->command, command_name))
			return currentcommand;
		currentcommand = currentcommand->next;
	}

	return NULL;
}

/*
 * This command should be removed.
 * The help functionality should be integrated into the cli_process_message function from climanager.c.
 */
int cli_help(int client_fd, char *args) {
	char msg[MAX_BUFFER_SIZE] = { 0 };
	struct command *currentcommand;
	int count = 1;

	currentcommand = head->next;
	//sprintf(msg, "\n Available command list are : \n");
	//cli_send_feedback(client_fd, msg);

	while (currentcommand) {

		if(currentcommand->hidden == false){
			sprintf(msg, "|[%d]: [%s]| \n", count, currentcommand->command);
			//cli_prompt(client_fd);
			cli_send_feedback(client_fd, msg);
			++count;
		}
		currentcommand = currentcommand->next;
	}

	return 0;
}

int cli_node_help(int client_fd, struct command_head *currentnode) {
	char msg[MAX_BUFFER_SIZE] = { 0 };
	struct command *currentcommand;
	int count = 1;

	currentcommand = currentnode->next;
	//sprintf(msg, "\n Available command list are : \n");
	//cli_send_feedback(client_fd, msg);

	while (currentcommand) {

		if(currentcommand->hidden == false){
			sprintf(msg, "[%d]:[%s]\n", count, currentcommand->command);
			cli_send_feedback(client_fd, msg);
			++count;
		}
		currentcommand = currentcommand->next;
	}

	return 0;
}

/*
 * Show the opennopd# prompt.
 */
#if 0
int cli_prompt(int client_fd){
	char msg[MAX_BUFFER_SIZE] = { 0 };
	sprintf(msg, "opennopd# ");
	cli_send_feedback(client_fd, msg);
	return 0;
}
#endif

/*
 * Testing params
 */
int cli_show_param(int client_fd, char **parameters, int numparameters) {
	int i = 0;
	char msg[MAX_BUFFER_SIZE] = { 0 };

	for (i=0;i<numparameters;i++){
		sprintf(msg, "[%d] %s\n", i, parameters[i]);
		cli_send_feedback(client_fd, msg);
	}

	return 0;
}

void bytestostringbps(char *output, __u32 count) {
	int I = 0;
	int D = 0;
	int bits = 0;
	bits = count * 8; // convert bytes to bps.

	if (bits < 1024) { // output as bits.
		sprintf(output, "%i bps", bits);

		return;
	}

	if (((bits / 1024) / 1024) >= 1024) { // output as Gbps.
		I = ((bits / 1024) / 1024) / 1024;
		D = (((bits / 1024) / 1024) % 1024) / 10;
		sprintf(output, "%i.%i Gbps", I, D);

		return;
	}

	if ((bits / 1024) >= 1024) { // output as Mbps.
		I = (bits / 1024) / 1024;
		D = ((bits / 1024) % 1024) / 10;
		sprintf(output, "%i.%i Mbps", I, D);

		return;
	}

	if (bits >= 1024) { // output as Kbps.
		I = bits / 1024;
		D = (bits % 1024) / 10;
		sprintf(output, "%i.%i Kbps", I, D);

		return;
	}

	return;
}

int cli_send_feedback(int client_fd, char *msg) {
#if 0
    char msg_send[LOGSZ] = { 0 };
    sprintf(msg_send, "Sending Mesage : %s", msg);
    logger(LOG_INFO, msg_send);
#endif
	if ((send(client_fd, msg, strlen(msg), 0)) <= 0) {
		perror("[cli_manager]: send");
		return 1;
	}
	return 0;
}
