#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>//
#include <string.h>
#include <assert.h>//
#include <sys/types.h>//
#include <sys/stat.h>
#include <sys/wait.h>

#define MAX_LINE 100

char *temp[MAX_LINE][MAX_LINE];
int row = 0;

int gettoken(char *buf, const char *token){
	int num = 0;
	char *flag = NULL;

	flag = buf + strspn(buf, token);
	if((temp[row][num] = strtok(flag, token)) == NULL)	return num;

	num = 1;

	while(1){
		if((temp[row][num] = strtok(NULL, token)) == NULL)	break;

		if(strcmp(temp[row][num], ";") == 0){
			temp[row][num] = NULL;
            row++;
            num=0;
            continue;
        }

		if(num == MAX_LINE-1)	return -1;

		num++;
	}
	return num;
}
int main(int argc, char **argv)
{
	int proc_status;
	pid_t pid;
	FILE *fp = NULL;

	fp = fopen( argv[1], "r" );

	if(fp == NULL){

		char buf[MAX_LINE];

		while(1){

			printf("prompt> ");
			memset(buf, 0x00, MAX_LINE);
			fgets(buf, MAX_LINE-1, stdin);
			if(strcmp(buf, "\n") == 0)		continue;
			if(strncmp(buf, "quit\n", 5) == 0)	break;
			buf[strlen(buf) -1] ='\0';

			gettoken(buf, " ");
			for(int j = 0; j<=row; j++){
				pid = fork();
				if(pid==0)
				{
					if(execvp(temp[j][0], temp[j]) < 0){
					fprintf(stderr, "exec not implemented\n");
					exit(0);
					}
				}
				if(pid>0)
				{
					wait(&proc_status);
				}
			}
			row = 0;
		}
	}
	else{

		char ftemp[MAX_LINE];

		while( !feof( fp ) )
		{
	        char *fbuf = fgets( ftemp, MAX_LINE-1, fp );

	        if(fbuf == NULL) break;

			printf("%s", fbuf);
			fbuf[strlen(fbuf) -1] ='\0';

			gettoken(fbuf, " ");
			for(int j = 0; j<=row; j++){

				pid = fork();
				if(pid==0)
				{
					if(execvp(temp[j][0], temp[j]) < 0){
					fprintf(stderr, "exec not implemented\n");
					exit(0);
					}
				}
				if(pid>0)
				{
					wait(&proc_status);
				}
			}
			row = 0;
		}
		fclose( fp );
	}
	return 0;
}

